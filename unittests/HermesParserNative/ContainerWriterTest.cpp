/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ContainerWriter.h"

#include "KindHash.h"
#include "gtest/gtest.h"

using namespace hermes;

namespace {

/// Read the \p i th uint32 field of the container header.
uint32_t headerField(const std::vector<uint8_t> &buf, size_t i) {
  uint32_t v;
  memcpy(&v, buf.data() + i * 4, sizeof(v));
  return v;
}

TEST(ContainerWriterTest, WritesMagicVersionAndHash) {
  NativeStringTable strings;
  std::vector<uint32_t> program;
  std::vector<PositionResult> positions;

  auto buf = writeContainer(program, positions, strings);

  EXPECT_EQ(0x484D5052u, headerField(buf, 0));
  EXPECT_EQ(1u, headerField(buf, 1));
  EXPECT_EQ(computeKindHash(), headerField(buf, 2));
}

TEST(ContainerWriterTest, ProgramRegionIsEightByteAligned) {
  NativeStringTable strings;
  std::vector<uint32_t> program{1, 2, 3};
  std::vector<PositionResult> positions;

  auto buf = writeContainer(program, positions, strings);

  uint32_t programOffset = headerField(buf, 3);
  EXPECT_EQ(48u, programOffset);
  EXPECT_EQ(0u, programOffset % 8);
}

TEST(ContainerWriterTest, RoundTripsProgramAndStrings) {
  NativeStringTable strings;
  strings.intern("foo");
  strings.intern("bar");
  std::vector<uint32_t> program{7, 8, 9};
  std::vector<PositionResult> positions;
  positions.emplace_back(3, PositionInfo::Kind::Start, 10, 20, 30);

  auto buf = writeContainer(program, positions, strings);

  uint32_t programOffset = headerField(buf, 3);
  EXPECT_EQ(3u, headerField(buf, 4));
  uint32_t first;
  memcpy(&first, buf.data() + programOffset, sizeof(first));
  EXPECT_EQ(7u, first);

  EXPECT_EQ(1u, headerField(buf, 6));

  EXPECT_EQ(2u, headerField(buf, 8));
  uint32_t strDataOffset = headerField(buf, 9);
  EXPECT_EQ(6u, headerField(buf, 10));
  std::string data((const char *)buf.data() + strDataOffset, 6);
  EXPECT_EQ("foobar", data);
}

TEST(ContainerWriterTest, StringOffsetArrayHasCountPlusOneEntries) {
  NativeStringTable strings;
  strings.intern("alpha");
  strings.intern("be");
  std::vector<uint32_t> program;
  std::vector<PositionResult> positions;

  auto buf = writeContainer(program, positions, strings);

  uint32_t strOffsetsOffset = headerField(buf, 7);
  uint32_t count = headerField(buf, 8);
  ASSERT_EQ(2u, count);

  std::vector<uint32_t> offsets(count + 1);
  memcpy(
      offsets.data(),
      buf.data() + strOffsetsOffset,
      (count + 1) * sizeof(uint32_t));
  EXPECT_EQ(0u, offsets[0]);
  EXPECT_EQ(5u, offsets[1]);
  EXPECT_EQ(7u, offsets[2]);
}

/// \c writeContainerInto() writes into memory the caller supplies, which for
/// the addon is a fresh ArrayBuffer whose contents are unspecified. Poison the
/// destination first and require that not one byte survives: anything left
/// over would be an uninitialized byte handed to JavaScript. Also require the
/// result to match \c writeContainer() exactly, so the two entry points cannot
/// drift apart.
TEST(ContainerWriterTest, WriteIntoCoversEveryByteOfPoisonedBuffer) {
  NativeStringTable strings;
  strings.intern("alpha");
  strings.intern("be");
  // Seven bytes of string data, so the container's total size is not a
  // multiple of four and the tail is the most likely place for a gap.
  std::vector<uint32_t> program{7, 8, 9, 10, 11};
  std::vector<PositionResult> positions;
  positions.emplace_back(3, PositionInfo::Kind::Start, 10, 20, 30);
  positions.emplace_back(4, PositionInfo::Kind::End, 11, 21, 31);

  const ContainerLayout layout = containerLayout(program, positions, strings);
  // 48 header + 20 program + 40 positions + 12 offsets + 7 data. Deliberately
  // not a multiple of four.
  ASSERT_EQ(127u, layout.total);

  std::vector<uint8_t> poisoned(layout.total, 0xAA);
  writeContainerInto(poisoned.data(), layout, program, positions, strings);

  const std::vector<uint8_t> expected =
      writeContainer(program, positions, strings);
  ASSERT_EQ(expected.size(), poisoned.size());
  EXPECT_EQ(expected, poisoned);
}

} // namespace
