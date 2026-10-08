/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/native_digest.h>
#include <hermes/node-compat/bundle/wasm_record.h>

#include "TempTree.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace hermes::node_compat;
using hermes::node_compat::test::TempTree;

namespace {

/// A kNativeDigestBytes-byte digest whose every byte is \p fill, distinct
/// enough for tests that need two or more digests to compare unequal and
/// sort predictably.
std::string makeDigest(uint8_t fill) {
  return std::string(kNativeDigestBytes, static_cast<char>(fill));
}

std::vector<uint8_t> readFileBytes(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  std::string s = ss.str();
  return std::vector<uint8_t>(s.begin(), s.end());
}

} // namespace

TEST(WasmRecordTest, FlushWithNoEntriesProducesAnEmptyValidFile) {
  TempTree tree;
  std::string path = tree.path("empty.wrec");
  WasmRecordWriter w(path, "hermes-node 1.2.3");
  ASSERT_TRUE(w.flush());

  std::vector<uint8_t> bytes = readFileBytes(path);
  ASSERT_FALSE(bytes.empty());

  std::string error;
  auto r = WasmRecordReader::open(bytes.data(), bytes.size(), &error);
  ASSERT_TRUE(r.has_value()) << error;
  EXPECT_EQ(r->count(), 0u);
  EXPECT_EQ(r->buildVersion(), "hermes-node 1.2.3");
}

TEST(WasmRecordTest, RoundTripsTwoEntries) {
  TempTree tree;
  std::string path = tree.path("two.wrec");
  WasmRecordWriter w(path, "hermes-node 1.2.3");

  std::string digestA = makeDigest(0xAA);
  std::string digestB = makeDigest(0xBB);
  std::string bytecodeA = "BYTECODE-A-SHORT";
  std::string bytecodeB = "BYTECODE-B-A-BIT-LONGER-THAN-A";

  ASSERT_TRUE(w.record(
      reinterpret_cast<const uint8_t *>(digestA.data()),
      reinterpret_cast<const uint8_t *>(bytecodeA.data()),
      bytecodeA.size()));
  ASSERT_TRUE(w.record(
      reinterpret_cast<const uint8_t *>(digestB.data()),
      reinterpret_cast<const uint8_t *>(bytecodeB.data()),
      bytecodeB.size()));

  std::vector<uint8_t> bytes = readFileBytes(path);
  std::string error;
  auto r = WasmRecordReader::open(bytes.data(), bytes.size(), &error);
  ASSERT_TRUE(r.has_value()) << error;
  EXPECT_EQ(r->count(), 2u);
  EXPECT_EQ(r->buildVersion(), "hermes-node 1.2.3");

  // Order is by digest, not insertion order, so look each one up rather
  // than assuming a position.
  bool foundA = false, foundB = false;
  for (uint32_t i = 0; i < r->count(); ++i) {
    if (r->digest(i) == digestA) {
      EXPECT_EQ(r->payload(i), bytecodeA);
      foundA = true;
    } else if (r->digest(i) == digestB) {
      EXPECT_EQ(r->payload(i), bytecodeB);
      foundB = true;
    }
  }
  EXPECT_TRUE(foundA);
  EXPECT_TRUE(foundB);
}

TEST(WasmRecordTest, RecordWithExistingDigestReplacesThePayload) {
  TempTree tree;
  std::string path = tree.path("replace.wrec");
  WasmRecordWriter w(path, "v1");

  std::string digest = makeDigest(0x42);
  std::string oldBytecode = "OLD-BYTECODE";
  std::string newBytecode = "REPLACEMENT-BYTECODE-DIFFERENT-LENGTH";

  ASSERT_TRUE(w.record(
      reinterpret_cast<const uint8_t *>(digest.data()),
      reinterpret_cast<const uint8_t *>(oldBytecode.data()),
      oldBytecode.size()));
  ASSERT_TRUE(w.record(
      reinterpret_cast<const uint8_t *>(digest.data()),
      reinterpret_cast<const uint8_t *>(newBytecode.data()),
      newBytecode.size()));

  std::vector<uint8_t> bytes = readFileBytes(path);
  std::string error;
  auto r = WasmRecordReader::open(bytes.data(), bytes.size(), &error);
  ASSERT_TRUE(r.has_value()) << error;
  ASSERT_EQ(r->count(), 1u);
  EXPECT_EQ(r->digest(0), digest);
  EXPECT_EQ(r->payload(0), newBytecode);
}

TEST(WasmRecordTest, RejectsBadMagic) {
  TempTree tree;
  WasmRecordWriter w(tree.path("bad-magic.wrec"), "v1");
  ASSERT_TRUE(w.flush());
  std::vector<uint8_t> bytes = readFileBytes(tree.path("bad-magic.wrec"));
  bytes[0] ^= 0xFF;

  std::string error;
  auto r = WasmRecordReader::open(bytes.data(), bytes.size(), &error);
  EXPECT_FALSE(r.has_value());
  EXPECT_FALSE(error.empty());
}

TEST(WasmRecordTest, RejectsFormatVersionMismatch) {
  TempTree tree;
  WasmRecordWriter w(tree.path("bad-version.wrec"), "v1");
  ASSERT_TRUE(w.flush());
  std::vector<uint8_t> bytes = readFileBytes(tree.path("bad-version.wrec"));

  uint32_t bogus = kWasmRecordFormatVersion + 1;
  std::memcpy(
      bytes.data() + offsetof(WasmRecordHeader, formatVersion),
      &bogus,
      sizeof(bogus));

  std::string error;
  auto r = WasmRecordReader::open(bytes.data(), bytes.size(), &error);
  EXPECT_FALSE(r.has_value());
  EXPECT_FALSE(error.empty());
}

TEST(WasmRecordTest, RejectsTruncationAtEveryLength) {
  TempTree tree;
  std::string path = tree.path("truncate.wrec");
  WasmRecordWriter w(path, "hermes-node 1.2.3");
  std::string digest = makeDigest(0x11);
  std::string bytecode = "SOME-BYTECODE-BYTES";
  ASSERT_TRUE(w.record(
      reinterpret_cast<const uint8_t *>(digest.data()),
      reinterpret_cast<const uint8_t *>(bytecode.data()),
      bytecode.size()));

  std::vector<uint8_t> bytes = readFileBytes(path);
  for (size_t n = 0; n < bytes.size(); ++n) {
    std::string error;
    auto r = WasmRecordReader::open(bytes.data(), n, &error);
    EXPECT_FALSE(r.has_value())
        << "accepted a truncated record file of " << n << " bytes";
    EXPECT_FALSE(error.empty()) << "no error message at length " << n;
  }
}

// The reader casts a pointer directly onto the record table
// (WasmRecordReader::open), which is undefined behavior at a misaligned
// address even where the CPU tolerates it. The writer always emits the
// table aligned; a corrupt or adversarial file might not. Following
// BundleFormatTest's RejectsMisalignedModuleTableOffset technique: a bogus
// header field alone is not enough to isolate this check (the
// reinterpret_cast would land on shifted-but-otherwise-valid-looking bytes
// and could fail some other check first), so the real table bytes are
// physically relocated to a misaligned file offset, keeping every other
// field byte-for-byte valid.
TEST(WasmRecordTest, RejectsMisalignedRecordTableOffset) {
  TempTree tree;
  std::string path = tree.path("misaligned.wrec");
  WasmRecordWriter w(path, "hermes-node 1.2.3");
  std::string digestA = makeDigest(0x01);
  std::string digestB = makeDigest(0x02);
  std::string bytecodeA = "AAAA";
  std::string bytecodeB = "BBBB";
  ASSERT_TRUE(w.record(
      reinterpret_cast<const uint8_t *>(digestA.data()),
      reinterpret_cast<const uint8_t *>(bytecodeA.data()),
      bytecodeA.size()));
  ASSERT_TRUE(w.record(
      reinterpret_cast<const uint8_t *>(digestB.data()),
      reinterpret_cast<const uint8_t *>(bytecodeB.data()),
      bytecodeB.size()));

  std::vector<uint8_t> good = readFileBytes(path);
  WasmRecordHeader header;
  std::memcpy(&header, good.data(), sizeof(header));
  size_t recordTableSize = header.count * sizeof(BundleWasmRecord);

  for (uint32_t delta = 1; delta <= 3; ++delta) {
    // Insert `delta` bytes right before the record table, pushing it (and
    // the payload after it) forward by `delta`, misaligning it; insert
    // `4 - delta` more bytes right after the table so the payload's own
    // shift is a full 4 bytes and it stays where it would ordinarily be
    // relative to the record table's own alignment requirement.
    std::vector<uint8_t> bytes = good;
    bytes.insert(bytes.begin() + header.recordTableOffset, delta, 0);
    bytes.insert(
        bytes.begin() + header.recordTableOffset + delta + recordTableSize,
        4 - delta,
        0);

    WasmRecordHeader patched = header;
    patched.recordTableOffset += delta;
    patched.payloadOffset += 4;
    std::memcpy(bytes.data(), &patched, sizeof(patched));

    std::string error;
    auto r = WasmRecordReader::open(bytes.data(), bytes.size(), &error);
    EXPECT_FALSE(r.has_value())
        << "accepted recordTableOffset misaligned by " << delta;
    EXPECT_FALSE(error.empty()) << "no error message for delta " << delta;
  }
}

TEST(WasmRecordTest, RejectsOutOfRangePayload) {
  TempTree tree;
  std::string path = tree.path("bad-payload.wrec");
  WasmRecordWriter w(path, "v1");
  std::string digest = makeDigest(0x33);
  std::string bytecode = "SOME-BYTECODE";
  ASSERT_TRUE(w.record(
      reinterpret_cast<const uint8_t *>(digest.data()),
      reinterpret_cast<const uint8_t *>(bytecode.data()),
      bytecode.size()));

  std::vector<uint8_t> bytes = readFileBytes(path);
  WasmRecordHeader header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  BundleWasmRecord record;
  std::memcpy(&record, bytes.data() + header.recordTableOffset, sizeof(record));
  // Claim a payload far bigger than the file actually has, without touching
  // header.payloadSize -- the check under test is the per-record range
  // against the payload section, not the section's own bounds.
  record.payloadSize = 0xFFFFFFFFu;
  std::memcpy(bytes.data() + header.recordTableOffset, &record, sizeof(record));

  std::string error;
  auto r = WasmRecordReader::open(bytes.data(), bytes.size(), &error);
  EXPECT_FALSE(r.has_value());
  EXPECT_FALSE(error.empty());
}

namespace {

WasmRecordCommit commitOf(
    const std::string &cfg,
    const std::string &bc,
    const std::string &srcDigest,
    const std::string &src) {
  return WasmRecordCommit{
      reinterpret_cast<const uint8_t *>(cfg.data()),
      reinterpret_cast<const uint8_t *>(bc.data()),
      bc.size(),
      reinterpret_cast<const uint8_t *>(srcDigest.data()),
      reinterpret_cast<const uint8_t *>(src.data()),
      src.size()};
}

/// A v1 file exactly as every hermes-node before format v2 wrote it.
std::vector<uint8_t> makeV1File(
    const std::string &version,
    const std::vector<std::pair<std::string, std::string>> &entries) {
  auto align = [](size_t n, size_t a) { return (n + a - 1) & ~(a - 1); };
  size_t tableOff = align(kWasmRecordHeaderV1Size + version.size(), 4);
  size_t payloadOff =
      align(tableOff + entries.size() * sizeof(BundleWasmRecord), 8);
  std::vector<uint8_t> payload;
  std::vector<BundleWasmRecord> table;
  for (const auto &[digest, bc] : entries) {
    BundleWasmRecord r{};
    std::memcpy(r.digest, digest.data(), kNativeDigestBytes);
    r.payloadOffset = static_cast<uint32_t>(payload.size());
    r.payloadSize = static_cast<uint32_t>(bc.size());
    table.push_back(r);
    payload.insert(payload.end(), bc.begin(), bc.end());
    payload.resize(align(payload.size(), 8), 0);
  }
  std::vector<uint8_t> out(payloadOff + payload.size(), 0);
  std::memcpy(out.data(), kWasmRecordMagic, 8);
  uint32_t fields[7] = {
      1,
      static_cast<uint32_t>(entries.size()),
      static_cast<uint32_t>(kWasmRecordHeaderV1Size),
      static_cast<uint32_t>(version.size()),
      static_cast<uint32_t>(tableOff),
      static_cast<uint32_t>(payloadOff),
      static_cast<uint32_t>(payload.size())};
  std::memcpy(out.data() + 8, fields, sizeof(fields));
  std::memcpy(
      out.data() + kWasmRecordHeaderV1Size, version.data(), version.size());
  std::memcpy(
      out.data() + tableOff,
      table.data(),
      table.size() * sizeof(BundleWasmRecord));
  std::memcpy(out.data() + payloadOff, payload.data(), payload.size());
  return out;
}

} // namespace

TEST(WasmRecordTest, Sha256DigestMatchesTheStandardVector) {
  const uint8_t abc[] = {'a', 'b', 'c'};
  EXPECT_EQ(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      nativeDigestToHex(sha256Digest(abc, sizeof(abc))));
}

TEST(WasmRecordTest, V2RoundTripsBytecodeAndSource) {
  TempTree tree;
  std::string path = tree.path("rec.bin");
  WasmRecordWriter w(path, "ver");
  ASSERT_TRUE(
      w.record(commitOf(makeDigest(1), "BYTECODE", makeDigest(9), "WASMSRC")));
  std::vector<uint8_t> bytes = readFileBytes(path);
  std::string err;
  auto r = WasmRecordReader::open(bytes.data(), bytes.size(), &err);
  ASSERT_TRUE(r) << err;
  EXPECT_EQ(2u, r->formatVersion());
  ASSERT_EQ(1u, r->count());
  EXPECT_EQ(makeDigest(1), r->digest(0));
  EXPECT_EQ("BYTECODE", r->payload(0));
  ASSERT_EQ(1u, r->sourceCount());
  EXPECT_EQ(makeDigest(9), r->sourceDigest(0));
  EXPECT_EQ("WASMSRC", r->source(0));
}

TEST(WasmRecordTest, OneSourceForTwoConfigurations) {
  TempTree tree;
  std::string path = tree.path("rec.bin");
  WasmRecordWriter w(path, "ver");
  ASSERT_TRUE(
      w.record(commitOf(makeDigest(1), "BC-T262-0", makeDigest(9), "SRC")));
  ASSERT_TRUE(
      w.record(commitOf(makeDigest(2), "BC-T262-1", makeDigest(9), "SRC")));
  std::vector<uint8_t> bytes = readFileBytes(path);
  auto r = WasmRecordReader::open(bytes.data(), bytes.size(), nullptr);
  ASSERT_TRUE(r);
  EXPECT_EQ(2u, r->count());
  EXPECT_EQ(1u, r->sourceCount());
}

TEST(WasmRecordTest, ReplacingBytecodeKeepsTheSource) {
  TempTree tree;
  std::string path = tree.path("rec.bin");
  WasmRecordWriter w(path, "ver");
  ASSERT_TRUE(w.record(commitOf(makeDigest(1), "OLD", makeDigest(9), "SRC")));
  ASSERT_TRUE(w.record(commitOf(makeDigest(1), "NEW", makeDigest(9), "SRC")));
  std::vector<uint8_t> bytes = readFileBytes(path);
  auto r = WasmRecordReader::open(bytes.data(), bytes.size(), nullptr);
  ASSERT_TRUE(r);
  ASSERT_EQ(1u, r->count());
  EXPECT_EQ("NEW", r->payload(0));
  ASSERT_EQ(1u, r->sourceCount());
  EXPECT_EQ("SRC", r->source(0));
}

TEST(WasmRecordTest, ReadsAV1FileForItsBytecode) {
  std::vector<uint8_t> v1 = makeV1File("ver", {{makeDigest(3), "BC3"}});
  std::string err;
  auto r = WasmRecordReader::open(v1.data(), v1.size(), &err);
  ASSERT_TRUE(r) << err;
  EXPECT_EQ(1u, r->formatVersion());
  ASSERT_EQ(1u, r->count());
  EXPECT_EQ("BC3", r->payload(0));
  EXPECT_EQ(0u, r->sourceCount());
}

TEST(WasmRecordTest, AnEmptyV1FileIsNeverReadPastItsEnd) {
  // Exactly the v1 file's bytes and no more: the v2 fields would sit past
  // its end, so reading them is a heap overflow under AddressSanitizer.
  std::vector<uint8_t> v1 = makeV1File("", {});
  ASSERT_LT(v1.size(), sizeof(WasmRecordHeader));
  auto *exact = static_cast<uint8_t *>(::malloc(v1.size()));
  ASSERT_NE(nullptr, exact);
  std::memcpy(exact, v1.data(), v1.size());
  std::string err;
  auto r = WasmRecordReader::open(exact, v1.size(), &err);
  ASSERT_TRUE(r) << err;
  EXPECT_EQ(1u, r->formatVersion());
  EXPECT_EQ(0u, r->count());
  EXPECT_EQ(0u, r->sourceCount());
  ::free(exact);
}

TEST(WasmRecordTest, RejectsAV2FileTruncatedToTheV1HeaderSize) {
  TempTree tree;
  std::string path = tree.path("rec.bin");
  WasmRecordWriter w(path, "");
  ASSERT_TRUE(w.flush());
  std::vector<uint8_t> bytes = readFileBytes(path);
  bytes.resize(kWasmRecordHeaderV1Size + 4);
  std::string err;
  EXPECT_FALSE(WasmRecordReader::open(bytes.data(), bytes.size(), &err));
  EXPECT_NE(std::string::npos, err.find("truncated"));
}

TEST(WasmRecordTest, RejectsAnUnknownLaterVersion) {
  std::vector<uint8_t> v1 = makeV1File("", {});
  uint32_t three = 3;
  std::memcpy(v1.data() + offsetof(WasmRecordHeader, formatVersion), &three, 4);
  std::string err;
  EXPECT_FALSE(WasmRecordReader::open(v1.data(), v1.size(), &err));
  EXPECT_NE(std::string::npos, err.find("version"));
}

TEST(WasmRecordTest, RejectsAMisalignedSourceTable) {
  // Moves the source table itself, as RejectsMisalignedRecordTableOffset
  // does for the record table: bumping the offset alone would make the
  // reader parse shifted bytes, and the payload-range check could then
  // reject the file without the alignment check ever running.
  TempTree tree;
  std::string path = tree.path("rec.bin");
  WasmRecordWriter w(path, "ver");
  ASSERT_TRUE(w.record(commitOf(makeDigest(1), "BC", makeDigest(9), "SRC")));
  std::vector<uint8_t> good = readFileBytes(path);
  WasmRecordHeader header;
  std::memcpy(&header, good.data(), sizeof(header));
  size_t tableSize = header.sourceCount * sizeof(WasmSourceRecord);

  for (uint32_t delta = 1; delta <= 3; ++delta) {
    // `delta` bytes before the table misalign it; `8 - delta` after it keep
    // the payload area's own shift a multiple of kBundlePayloadAlign.
    std::vector<uint8_t> bytes = good;
    bytes.insert(bytes.begin() + header.sourceTableOffset, delta, 0);
    bytes.insert(
        bytes.begin() + header.sourceTableOffset + delta + tableSize,
        8 - delta,
        0);
    WasmRecordHeader patched = header;
    patched.sourceTableOffset += delta;
    patched.payloadOffset += 8;
    std::memcpy(bytes.data(), &patched, sizeof(patched));

    std::string err;
    EXPECT_FALSE(WasmRecordReader::open(bytes.data(), bytes.size(), &err))
        << "accepted a source table misaligned by " << delta;
    EXPECT_NE(std::string::npos, err.find("source table offset is misaligned"))
        << err;
  }
}

TEST(WasmRecordTest, RejectsAnOutOfRangeSourcePayload) {
  TempTree tree;
  std::string path = tree.path("rec.bin");
  WasmRecordWriter w(path, "ver");
  ASSERT_TRUE(w.record(commitOf(makeDigest(1), "BC", makeDigest(9), "SRC")));
  std::vector<uint8_t> bytes = readFileBytes(path);
  uint32_t tableOff;
  std::memcpy(
      &tableOff,
      bytes.data() + offsetof(WasmRecordHeader, sourceTableOffset),
      4);
  uint32_t huge = 0x7fffffff;
  std::memcpy(
      bytes.data() + tableOff + offsetof(WasmSourceRecord, payloadSize),
      &huge,
      4);
  std::string err;
  EXPECT_FALSE(WasmRecordReader::open(bytes.data(), bytes.size(), &err));
  EXPECT_NE(std::string::npos, err.find("source"));
}
