/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/wasm_record.h>

#include "TempTree.h"

#include <gtest/gtest.h>

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
