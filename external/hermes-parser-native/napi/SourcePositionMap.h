/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_TOOLS_HERMESPARSERNATIVE_SOURCEPOSITIONMAP_H
#define HERMES_TOOLS_HERMESPARSERNATIVE_SOURCEPOSITIONMAP_H

#include <cstdint>
#include <cstring>
#include <vector>

#include "llvh/ADT/StringRef.h"

namespace hermes {

/// Converts byte offsets into a UTF-8 source buffer to the (line, column,
/// offset) triples the JavaScript side expects, in O(1) per query after one
/// O(N) pass over the buffer.
///
/// The triples are indices into a JavaScript *string*, so a "column" and the
/// overall "offset" count UTF-16 code units, not bytes and not code points:
/// code points up to U+FFFF count as one, code points above U+FFFF count as
/// two because they need a surrogate pair. Lines are 1-based and columns are
/// 0-based, and only '\n' ends a line ('\r' is an ordinary character, so a
/// CRLF file has the '\r' as the last column of the line).
///
/// This exists to replace a global sort. The obvious way to compute these
/// triples is to sort every recorded position by its address and then walk
/// the source once, since that walk never has to rewind; but the sort costs
/// O(P log P) over ~2 positions per AST node and dominates everything else
/// for large inputs. Here the O(N) walk happens once up front and records
/// enough state that any position can be answered on its own, in any order.
///
/// The walk produces:
///
/// - \c lines_, one entry per line, holding the line's byte offset and its
///   UTF-16 offset.
/// - \c mbStarts_ / \c mbDeltas_, one entry per *multi-byte* character,
///   holding where it starts and how far byte offsets have run ahead of
///   UTF-16 offsets after it. These are empty for pure-ASCII sources - the
///   overwhelmingly common case - and then a byte offset *is* its UTF-16
///   offset and there is nothing to look up.
/// - \c chunks_ and \c mbChunks_, mapping each fixed-size block of the source
///   to where in the two tables above that block begins. They turn both
///   lookups into a direct index plus a walk over the few entries the block
///   covers, instead of a binary search whose every probe is a fresh cache
///   miss in a table with an entry per line, or per character.
///
/// Everything is stored as byte offsets rather than pointers so that the
/// tables stay half the size on 64-bit hosts; the source buffers this runs
/// on are a single JavaScript string and cannot exceed 4 GB.
class SourcePositionMap {
 public:
  /// A source position expressed the way the JavaScript side wants it.
  struct Position {
    /// 1-based line number.
    uint32_t line;
    /// 0-based column, counted in UTF-16 code units from the line start.
    uint32_t column;
    /// 0-based offset into the whole source, in UTF-16 code units.
    uint32_t offset;
  };

  /// Build the map for \p buffer, which must outlive this object.
  explicit SourcePositionMap(llvh::StringRef buffer)
      : data_(buffer.data()), size_((uint32_t)buffer.size()) {
    build();
  }

  /// \return the line, column and UTF-16 offset of \p byteOffset, which must
  /// be in [0, size()].
  Position lookup(uint32_t byteOffset) const {
    // Convert to a UTF-16 offset. For a position that is not on a character
    // boundary - only reachable with malformed UTF-8 - round forward to the
    // end of the character containing it, which is what a single forward
    // scan stopping at the first boundary >= byteOffset would report.
    uint32_t boundary = byteOffset;
    uint32_t offset = byteOffset;
    if (!mbStarts_.empty()) {
      // The first multi-byte character starting at or after \p byteOffset,
      // found the same way as the line below: index into a chunk table, then
      // walk forward over the few entries that chunk covers. A binary search
      // over the whole table would be a fresh cache miss per probe, and on a
      // source that is mostly non-ASCII that table has an entry per
      // character.
      size_t k = mbChunks_[byteOffset >> kChunkShift];
      while (k != mbStarts_.size() && mbStarts_[k] < byteOffset) {
        ++k;
      }
      if (k != 0) {
        const uint32_t prevStart = mbStarts_[k - 1];
        const uint32_t prevEnd = prevStart + charWidth(data_[prevStart]);
        if (byteOffset < prevEnd) {
          boundary = prevEnd;
        }
        offset = boundary - mbDeltas_[k - 1];
      }
    }

    // Find the line containing the boundary. A line start can never fall
    // strictly inside a multi-byte character, so looking up \p byteOffset or
    // the rounded boundary gives the same line.
    size_t chunk = (size_t)(boundary >> kChunkShift);
    if (chunk >= chunks_.size()) {
      chunk = chunks_.size() - 1;
    }
    size_t line = chunks_[chunk];
    while (line + 1 < lines_.size() && lines_[line + 1].byteStart <= boundary) {
      ++line;
    }

    return Position{
        (uint32_t)line + 1, offset - lines_[line].utf16Start, offset};
  }

  /// \return the size of the source buffer in bytes.
  uint32_t size() const {
    return size_;
  }

  /// \return true if the source contains no byte >= 0x80, in which case byte
  /// offsets and UTF-16 offsets coincide. Exposed for testing.
  bool isPureAscii() const {
    return mbStarts_.empty();
  }

 private:
  /// Where a line begins, in both units.
  struct LineInfo {
    /// Byte offset of the first character of the line.
    uint32_t byteStart;
    /// UTF-16 offset of the first character of the line.
    uint32_t utf16Start;
  };

  /// log2 of the block size the chunk tables are indexed by. 64 bytes is a
  /// little over one source line of typical code, so the walks in
  /// \c lookup() take a step or two, while each table stays at 1/16th of the
  /// size of the source. Measured against 256-byte blocks, which halve the
  /// table but lengthen the walk, 64 is 2-4% faster end to end.
  static constexpr unsigned kChunkShift = 6;

  /// \return how many bytes the character led by \p lead occupies, using
  /// exactly the classification the single-pass scan this replaces used, so
  /// that malformed input is walked the same way rather than better.
  static uint32_t charWidth(char lead) {
    if ((unsigned char)lead < 128) {
      return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
      return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
      return 3;
    }
    return 4;
  }

  /// \return the offset of the first byte >= 0x80, or \c size_ if there is
  /// none. Scanned a machine word at a time, since for most sources the
  /// answer is "there is none" and this is the only pass that has to look at
  /// every byte.
  uint32_t findFirstNonAscii() const {
    uint32_t i = 0;
    while (i + sizeof(uint64_t) <= size_) {
      uint64_t word;
      memcpy(&word, data_ + i, sizeof(word));
      if ((word & 0x8080808080808080ull) != 0) {
        break;
      }
      i += (uint32_t)sizeof(uint64_t);
    }
    for (; i != size_; ++i) {
      if ((unsigned char)data_[i] >= 0x80) {
        return i;
      }
    }
    return size_;
  }

  /// Record a line start for every '\n' in [\p begin, \p end), which must
  /// contain no byte >= 0x80 so that byte and UTF-16 offsets coincide.
  void buildAsciiLines(uint32_t begin, uint32_t end) {
    const char *p = data_ + begin;
    const char *const limit = data_ + end;
    while (p != limit) {
      const char *nl = (const char *)memchr(p, '\n', (size_t)(limit - p));
      if (nl == nullptr) {
        return;
      }
      const uint32_t start = (uint32_t)(nl + 1 - data_);
      lines_.push_back(LineInfo{start, start});
      p = nl + 1;
    }
  }

  /// Walk the buffer once and populate every table.
  void build() {
    lines_.push_back(LineInfo{0, 0});

    // Everything up to the first non-ASCII byte is one byte per UTF-16 code
    // unit, so only the line starts matter there and memchr can find them.
    const uint32_t firstNonAscii = findFirstNonAscii();
    buildAsciiLines(0, firstNonAscii);

    // From there on, characters have to be decoded one at a time. For a
    // pure-ASCII source - the overwhelmingly common case - this loop does
    // not run at all and both multi-byte tables stay empty.
    uint32_t utf16 = firstNonAscii;
    uint32_t i = firstNonAscii;
    while (i < size_) {
      const unsigned char ch = (unsigned char)data_[i];
      if (ch < 128) {
        ++utf16;
        ++i;
        if (ch == '\n') {
          lines_.push_back(LineInfo{i, utf16});
        }
        continue;
      }

      // A multi-byte character. Two-byte and three-byte sequences are one
      // UTF-16 code unit; everything else is treated as a four-byte sequence
      // encoding a code point above U+FFFF, hence a surrogate pair.
      uint32_t width;
      uint32_t units;
      if ((data_[i] & 0xE0) == 0xC0) {
        width = 2;
        units = 1;
      } else if ((data_[i] & 0xF0) == 0xE0) {
        width = 3;
        units = 1;
      } else {
        width = 4;
        units = 2;
      }
      mbStarts_.push_back(i);
      utf16 += units;
      i += width;
      mbDeltas_.push_back(i - utf16);
    }

    // Index the line table by fixed-size blocks. The extra entries past the
    // last block cover positions rounded forward past the end of the buffer,
    // which malformed UTF-8 can produce.
    const size_t chunkCount = (size_t)(size_ >> kChunkShift) + 2;
    chunks_.resize(chunkCount);
    size_t line = 0;
    for (size_t c = 0; c != chunkCount; ++c) {
      const uint32_t base = (uint32_t)(c << kChunkShift);
      while (line + 1 < lines_.size() && lines_[line + 1].byteStart <= base) {
        ++line;
      }
      chunks_[c] = (uint32_t)line;
    }

    // Index the multi-byte table the same way, but only when there is one.
    if (!mbStarts_.empty()) {
      mbChunks_.resize(chunkCount);
      size_t k = 0;
      for (size_t c = 0; c != chunkCount; ++c) {
        const uint32_t base = (uint32_t)(c << kChunkShift);
        while (k != mbStarts_.size() && mbStarts_[k] < base) {
          ++k;
        }
        mbChunks_[c] = (uint32_t)k;
      }
    }
  }

  /// The source buffer. Not owned.
  const char *data_;
  /// Size of \c data_ in bytes.
  uint32_t size_;
  /// One entry per line, in increasing order of \c byteStart.
  std::vector<LineInfo> lines_;
  /// For each block of 1 << kChunkShift bytes, the index in \c lines_ of the
  /// last line starting at or before the block.
  std::vector<uint32_t> chunks_;
  /// Byte offset of each multi-byte character, in increasing order. Empty
  /// for pure-ASCII sources.
  std::vector<uint32_t> mbStarts_;
  /// For each entry of \c mbStarts_, how far the byte offset exceeds the
  /// UTF-16 offset immediately after that character.
  std::vector<uint32_t> mbDeltas_;
  /// For each block of 1 << kChunkShift bytes, the index in \c mbStarts_ of
  /// the first multi-byte character starting at or after the block. Empty
  /// exactly when \c mbStarts_ is, and only read when it is not.
  std::vector<uint32_t> mbChunks_;
};

} // namespace hermes

#endif
