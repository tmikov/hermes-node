/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StringTable.h"

#include "gtest/gtest.h"

using namespace hermes;

namespace {

TEST(StringTableTest, EmptyTableHasOneOffset) {
  NativeStringTable table;
  EXPECT_EQ(0u, table.count());
  ASSERT_EQ(1u, table.offsets().size());
  EXPECT_EQ(0u, table.offsets()[0]);
}

TEST(StringTableTest, InternReturnsSequentialIds) {
  NativeStringTable table;
  EXPECT_EQ(0u, table.intern("foo"));
  EXPECT_EQ(1u, table.intern("bar"));
  EXPECT_EQ(2u, table.count());
}

TEST(StringTableTest, InternDeduplicates) {
  NativeStringTable table;
  EXPECT_EQ(0u, table.intern("foo"));
  EXPECT_EQ(1u, table.intern("bar"));
  EXPECT_EQ(0u, table.intern("foo"));
  EXPECT_EQ(2u, table.count());
  EXPECT_EQ("foobar", table.data());
}

TEST(StringTableTest, OffsetsDelimitStrings) {
  NativeStringTable table;
  table.intern("alpha");
  table.intern("be");
  ASSERT_EQ(3u, table.offsets().size());
  EXPECT_EQ(0u, table.offsets()[0]);
  EXPECT_EQ(5u, table.offsets()[1]);
  EXPECT_EQ(7u, table.offsets()[2]);
}

TEST(StringTableTest, HandlesEmptyString) {
  NativeStringTable table;
  EXPECT_EQ(0u, table.intern(""));
  EXPECT_EQ(1u, table.count());
  EXPECT_EQ(0u, table.offsets()[0]);
  EXPECT_EQ(0u, table.offsets()[1]);
}

TEST(StringTableTest, HandlesEmbeddedNulAndUtf8) {
  NativeStringTable table;
  llvh::StringRef withNul("a\0b", 3);
  EXPECT_EQ(0u, table.intern(withNul));
  EXPECT_EQ(1u, table.intern("\xC3\xA9"));
  EXPECT_EQ(3u, table.offsets()[1]);
  EXPECT_EQ(5u, table.offsets()[2]);
}

} // namespace
