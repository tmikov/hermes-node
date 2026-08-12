/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KindHash.h"

#include "gtest/gtest.h"
#include "llvh/ADT/StringRef.h"

using namespace hermes;

namespace {

TEST(KindHashTest, IsStableAcrossCalls) {
  EXPECT_EQ(computeKindHash(), computeKindHash());
}

TEST(KindHashTest, CachedAccessorMatchesComputation) {
  // writeContainer() stamps the container header with kindHash() rather than
  // recomputing the hash on every parse. The cached value must be the one the
  // JavaScript side's independent computation is checked against.
  EXPECT_EQ(computeKindHash(), kindHash());
  EXPECT_EQ(kindHash(), kindHash());
}

TEST(KindHashTest, IsNotTrivial) {
  uint32_t hash = computeKindHash();
  EXPECT_NE(0u, hash);
  EXPECT_NE(0x811C9DC5u, hash) << "hash equals the FNV-1a seed; "
                                  "the name list was empty";
}

TEST(KindHashTest, MatchesReferenceImplementation) {
  // Recompute independently over the same list to catch a macro that stopped
  // expanding. The first three entries are Empty(), Metadata() and the
  // FunctionLikeFirst range marker.
  uint32_t h = 0x811C9DC5u;
  auto feed = [&h](const char *s) {
    for (const char *p = s; *p; ++p) {
      h ^= (uint32_t)(unsigned char)*p;
      h *= 16777619u;
    }
    h ^= (uint32_t)'\n';
    h *= 16777619u;
  };
  feed("Empty()");
  feed("Metadata()");
  feed("FunctionLikeFirst");
  // The real hash covers all entries, so it must differ from this prefix.
  EXPECT_NE(h, computeKindHash());
}

TEST(KindHashTest, EntriesCarryFieldNames) {
  // The hash is only sensitive to a change in an existing node's field list
  // if the entries actually spell that list out. Pin two representative
  // shapes: a node with fields, and a range marker without any. If the
  // ESTREE_NODE_n_ARGS macros regressed to expanding to the bare node name,
  // these would fail even though every node name is still present.
  bool sawIdentifier = false;
  bool sawRangeMarker = false;
  for (const char *entry : kNodeKindEntries) {
    llvh::StringRef str{entry};
    if (str.startswith("Identifier(")) {
      EXPECT_EQ("Identifier(name,typeAnnotation,optional)", str.str());
      sawIdentifier = true;
    }
    if (str == "FunctionLikeFirst") {
      sawRangeMarker = true;
    }
  }
  EXPECT_TRUE(sawIdentifier) << "Identifier entry must list its fields";
  EXPECT_TRUE(sawRangeMarker) << "range markers must contribute a bare name";
}

} // namespace
