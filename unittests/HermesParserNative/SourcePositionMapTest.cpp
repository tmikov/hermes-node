/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SourcePositionMap.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

using namespace hermes;

namespace {

/// The algorithm \c SourcePositionMap replaces, kept here as the oracle every
/// test compares against: walk the source from the start one character at a
/// time, stopping at the first character boundary at or after \p byteOffset,
/// and report the state there. This is a transcription of the loop that used
/// to live in HermesParserJSSerializer::serializeSourcePositions(), so a
/// disagreement between the two is exactly the kind of off-by-one that would
/// silently corrupt every `loc` in the AST.
SourcePositionMap::Position referenceLookup(
    llvh::StringRef source,
    uint32_t byteOffset) {
  const char *ptr = source.data();
  const char *const nextLoc = source.data() + byteOffset;

  uint32_t line = 1;
  uint32_t col = 0;
  uint32_t offset = 0;
  while (ptr < nextLoc) {
    char ch = *ptr;
    if (ch == '\n') {
      ++offset;
      ++line;
      col = 0;
      ++ptr;
    } else if ((unsigned char)ch < 128) {
      ++offset;
      ++col;
      ++ptr;
    } else if ((ch & 0xE0) == 0xC0) {
      ++offset;
      ++col;
      ptr += 2;
    } else if ((ch & 0xF0) == 0xE0) {
      ++offset;
      ++col;
      ptr += 3;
    } else {
      offset += 2;
      col += 2;
      ptr += 4;
    }
  }
  return SourcePositionMap::Position{line, col, offset};
}

/// Check every byte offset in \p source, from 0 through the end of the
/// buffer inclusive, against \c referenceLookup().
///
/// \c referenceLookup() restarts the walk for every offset, which is
/// quadratic and unusable on the longer sources below, so the walk is run
/// once here instead and each of its states is checked against every offset
/// that resolves to it. The two agree by construction: \c referenceLookup()
/// stops at the first boundary at or after the offset, so all the offsets in
/// (previous boundary, this boundary] share this boundary's state.
void expectMatchesReference(const std::string &source) {
  const llvh::StringRef ref(source);
  SourcePositionMap map(ref);
  ASSERT_EQ(map.size(), source.size());

  const uint32_t size = (uint32_t)source.size();
  uint32_t line = 1;
  uint32_t col = 0;
  uint32_t offset = 0;
  uint32_t boundary = 0;
  int64_t prevBoundary = -1;
  while (true) {
    for (int64_t i = prevBoundary + 1; i <= (int64_t)boundary; ++i) {
      if (i > size) {
        break;
      }
      const SourcePositionMap::Position got = map.lookup((uint32_t)i);
      ASSERT_EQ(line, got.line) << "line at byte " << i;
      ASSERT_EQ(col, got.column) << "column at byte " << i;
      ASSERT_EQ(offset, got.offset) << "offset at byte " << i;
    }
    if (boundary >= size) {
      break;
    }

    // Advance one character, exactly the way the old scan did.
    const char ch = source[boundary];
    prevBoundary = boundary;
    if (ch == '\n') {
      ++offset;
      ++line;
      col = 0;
      boundary += 1;
    } else if ((unsigned char)ch < 128) {
      ++offset;
      ++col;
      boundary += 1;
    } else if ((ch & 0xE0) == 0xC0) {
      ++offset;
      ++col;
      boundary += 2;
    } else if ((ch & 0xF0) == 0xE0) {
      ++offset;
      ++col;
      boundary += 3;
    } else {
      offset += 2;
      col += 2;
      boundary += 4;
    }
  }
}

/// Spot-check that the incremental walk above agrees with the literal
/// \c referenceLookup(), on a source small enough for the quadratic form.
void expectWalkAgreesWithLiteralReference(const std::string &source) {
  const llvh::StringRef ref(source);
  SourcePositionMap map(ref);
  for (uint32_t i = 0; i <= source.size(); ++i) {
    const SourcePositionMap::Position want = referenceLookup(ref, i);
    const SourcePositionMap::Position got = map.lookup(i);
    EXPECT_EQ(want.line, got.line) << "line at byte " << i;
    EXPECT_EQ(want.column, got.column) << "column at byte " << i;
    EXPECT_EQ(want.offset, got.offset) << "offset at byte " << i;
  }
}

TEST(SourcePositionMapTest, EmptySource) {
  SourcePositionMap map{llvh::StringRef("")};
  EXPECT_TRUE(map.isPureAscii());
  const SourcePositionMap::Position pos = map.lookup(0);
  EXPECT_EQ(1u, pos.line);
  EXPECT_EQ(0u, pos.column);
  EXPECT_EQ(0u, pos.offset);
}

TEST(SourcePositionMapTest, LinesAreOneBasedAndColumnsZeroBased) {
  const std::string source = "ab\ncd\n";
  SourcePositionMap map{llvh::StringRef(source)};

  // 'a' is line 1 column 0; the '\n' after "ab" is line 1 column 2; 'c' is
  // line 2 column 0.
  EXPECT_EQ(1u, map.lookup(0).line);
  EXPECT_EQ(0u, map.lookup(0).column);
  EXPECT_EQ(1u, map.lookup(2).line);
  EXPECT_EQ(2u, map.lookup(2).column);
  EXPECT_EQ(2u, map.lookup(3).line);
  EXPECT_EQ(0u, map.lookup(3).column);
  EXPECT_EQ(3u, map.lookup(3).offset);
}

TEST(SourcePositionMapTest, AstralCharacterCountsAsTwoCodeUnits) {
  // U+1F600 is four UTF-8 bytes and a surrogate pair in UTF-16.
  const std::string source =
      "a\xF0\x9F\x98\x80"
      "b";
  SourcePositionMap map{llvh::StringRef(source)};
  EXPECT_FALSE(map.isPureAscii());

  // 'b' is at byte 5, but at UTF-16 column 3: 'a' plus two surrogates.
  EXPECT_EQ(3u, map.lookup(5).column);
  EXPECT_EQ(3u, map.lookup(5).offset);
  EXPECT_EQ(1u, map.lookup(5).line);
}

TEST(SourcePositionMapTest, BmpMultiByteCharacterCountsAsOneCodeUnit) {
  // U+00E9 is two UTF-8 bytes, U+4E16 is three, both one UTF-16 code unit.
  const std::string source = "\xC3\xA9\xE4\xB8\x96z";
  SourcePositionMap map{llvh::StringRef(source)};
  EXPECT_EQ(2u, map.lookup(5).column);
  EXPECT_EQ(2u, map.lookup(5).offset);
}

TEST(SourcePositionMapTest, CarriageReturnDoesNotEndALine) {
  // Only '\n' ends a line, so in CRLF text the '\r' is the last column.
  const std::string source = "ab\r\ncd";
  SourcePositionMap map{llvh::StringRef(source)};
  EXPECT_EQ(1u, map.lookup(2).line);
  EXPECT_EQ(2u, map.lookup(2).column);
  EXPECT_EQ(1u, map.lookup(3).line);
  EXPECT_EQ(3u, map.lookup(3).column);
  EXPECT_EQ(2u, map.lookup(4).line);
  EXPECT_EQ(0u, map.lookup(4).column);

  // A lone '\r' is an ordinary character.
  const std::string cr = "ab\rcd";
  SourcePositionMap crMap{llvh::StringRef(cr)};
  EXPECT_EQ(1u, crMap.lookup(4).line);
  EXPECT_EQ(4u, crMap.lookup(4).column);
}

TEST(SourcePositionMapTest, PureAsciiIsDetected) {
  EXPECT_TRUE(SourcePositionMap{llvh::StringRef("var x = 1;\n")}.isPureAscii());
  EXPECT_FALSE(SourcePositionMap{llvh::StringRef("var x = '\xC3\xA9';\n")}
                   .isPureAscii());
}

TEST(SourcePositionMapTest, MatchesReferenceOnEdgeCaseSources) {
  const std::vector<std::string> sources = {
      "",
      "\n",
      "a",
      "a\n",
      "no trailing newline",
      "trailing newline\n",
      "\n\n\n\n",
      "a\r\nb\r\nc\r\n",
      "a\rb\rc",
      "\xC3\xA9",
      "\xE4\xB8\x96\xE7\x95\x8C",
      "\xF0\x9F\x98\x80",
      "\xF0\x9F\x98\x80\n\xF0\x9F\x9A\x80",
      "a\xC3\xA9\x62\xE4\xB8\x96\x63\xF0\x9F\x98\x80\x64",
      "\xEF\xBB\xBFvar a = 1;\n",
      "line1\n\xF0\x9F\x98\x80\nline3\n",
      // Continuation byte with no lead byte, and a truncated sequence: the
      // map must walk malformed input exactly the way the old scan did.
      "a\x80\x62",
      "a\xF0\x9F\x62",
      "a\xC3",
  };
  for (const std::string &source : sources) {
    SCOPED_TRACE(
        ::testing::Message() << "source of " << source.size() << " bytes");
    expectMatchesReference(source);
    expectWalkAgreesWithLiteralReference(source);
  }
}

TEST(SourcePositionMapTest, MatchesReferenceAcrossChunkBoundaries) {
  // Longer than one chunk table block, with line lengths that put line starts
  // on either side of a block boundary.
  for (size_t lineLen = 1; lineLen < 200; lineLen += 37) {
    std::string source;
    for (size_t line = 0; line != 40; ++line) {
      source.append(lineLen, 'x');
      source.push_back('\n');
    }
    SCOPED_TRACE(::testing::Message() << "line length " << lineLen);
    expectMatchesReference(source);
  }
}

TEST(SourcePositionMapTest, MatchesReferenceWithNonAsciiFarIntoALongLine) {
  // A single very long line whose only multi-byte character sits well past
  // the start, which is the shape that a per-line rescan would handle in
  // time quadratic in the line length.
  std::string source(5000, 'a');
  source += "\xF0\x9F\x98\x80";
  source.append(5000, 'b');
  source += "\n";
  expectMatchesReference(source);
}

TEST(SourcePositionMapTest, MatchesReferenceOnDenselyNonAsciiSource) {
  std::string source;
  for (size_t i = 0; i != 300; ++i) {
    source += "\xE4\xB8\x96\xF0\x9F\x98\x80\xC3\xA9";
    if (i % 7 == 0) {
      source += "\n";
    }
  }
  expectMatchesReference(source);
}

} // namespace
