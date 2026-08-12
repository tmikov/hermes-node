/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_TOOLS_HERMESPARSERNATIVE_CONTAINERWRITER_H
#define HERMES_TOOLS_HERMESPARSERNATIVE_CONTAINERWRITER_H

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include "HermesParserJSSerializer.h"
#include "KindHash.h"
#include "StringTable.h"

namespace hermes {

/// The position region is written by memcpy-ing raw \c PositionResult objects,
/// and the JavaScript deserializer reads it back as exactly five consecutive
/// uint32 words per entry (HermesParserDeserializer.fillLocs). Both properties
/// are pinned here because nothing else on either side enforces them: adding a
/// field or letting the compiler insert padding would silently shift every
/// subsequent position, and the container's own bounds checks would still
/// pass.
static_assert(
    sizeof(PositionResult) == 20,
    "PositionResult must be exactly five uint32 words; the JavaScript "
    "deserializer reads 5 words per position entry");
static_assert(
    std::is_trivially_copyable<PositionResult>::value,
    "PositionResult is memcpy-ed into the container, so it must be "
    "trivially copyable");

/// Size of the container header in bytes. Chosen so that the program region,
/// which immediately follows, starts 8-byte aligned. The JavaScript
/// deserializer creates a Float64Array view over that region, which requires
/// 8-byte alignment, and its number-padding logic depends on index parity
/// being preserved.
static constexpr uint32_t kHeaderSize = 48;

/// Magic value identifying a hermes-parser-native container: 'HMPR'.
static constexpr uint32_t kContainerMagic = 0x484D5052;

/// Version of the container layout. Bump on any incompatible change.
static constexpr uint32_t kContainerVersion = 1;

/// The byte offsets and sizes of every container region, derived from the
/// sizes of the three inputs. Computed by \c containerLayout() so that the
/// destination buffer can be allocated before anything is written into it.
struct ContainerLayout {
  /// Byte size of the program region.
  uint32_t programBytes;
  /// Byte size of the position region.
  uint32_t positionBytes;
  /// Byte size of the string offset table.
  uint32_t strOffsetsBytes;
  /// Byte size of the string data blob.
  uint32_t strDataBytes;
  /// Byte offset of the program region from the start of the container.
  uint32_t programOffset;
  /// Byte offset of the position region.
  uint32_t positionOffset;
  /// Byte offset of the string offset table.
  uint32_t strOffsetsOffset;
  /// Byte offset of the string data blob.
  uint32_t strDataOffset;
  /// Total byte size of the container.
  uint32_t total;
};

/// \return the region offsets and the total byte size the container for
/// \p program, \p positions and \p strings will occupy. The regions are laid
/// out back to back with no gaps, so \c total is exactly the number of bytes
/// \c writeContainerInto() writes.
inline ContainerLayout containerLayout(
    const std::vector<uint32_t> &program,
    const std::vector<PositionResult> &positions,
    const NativeStringTable &strings) {
  ContainerLayout l;
  l.programBytes = (uint32_t)(program.size() * sizeof(uint32_t));
  l.positionBytes = (uint32_t)(positions.size() * sizeof(PositionResult));
  l.strOffsetsBytes = (uint32_t)(strings.offsets().size() * sizeof(uint32_t));
  l.strDataBytes = (uint32_t)strings.data().size();

  l.programOffset = kHeaderSize;
  l.positionOffset = l.programOffset + l.programBytes;
  l.strOffsetsOffset = l.positionOffset + l.positionBytes;
  l.strDataOffset = l.strOffsetsOffset + l.strOffsetsBytes;
  l.total = l.strDataOffset + l.strDataBytes;
  return l;
}

/// Serialize \p program, \p positions and \p strings into \p dst, which must
/// point at \c containerLayout(...).total writable bytes and is passed as
/// \p layout to save recomputing it. See the header table in the design spec
/// for the field layout.
///
/// Every one of those bytes is written exactly once: the header covers
/// [0, kHeaderSize) and the four regions tile [kHeaderSize, total)
/// contiguously, so \p dst does not need to be zero-initialized first. That
/// is the whole point of this entry point - it lets the caller write straight
/// into an ArrayBuffer instead of filling a zeroed vector and copying it.
inline void writeContainerInto(
    void *dst,
    const ContainerLayout &layout,
    const std::vector<uint32_t> &program,
    const std::vector<PositionResult> &positions,
    const NativeStringTable &strings) {
  uint8_t *const buf = (uint8_t *)dst;

  const uint32_t header[] = {
      kContainerMagic,
      kContainerVersion,
      kindHash(),
      layout.programOffset,
      (uint32_t)program.size(),
      layout.positionOffset,
      (uint32_t)positions.size(),
      layout.strOffsetsOffset,
      strings.count(),
      layout.strDataOffset,
      layout.strDataBytes,
      0,
  };
  static_assert(sizeof(header) == kHeaderSize, "header size mismatch");
  memcpy(buf, header, sizeof(header));

  if (layout.programBytes != 0) {
    memcpy(buf + layout.programOffset, program.data(), layout.programBytes);
  }
  if (layout.positionBytes != 0) {
    memcpy(buf + layout.positionOffset, positions.data(), layout.positionBytes);
  }
  if (layout.strOffsetsBytes != 0) {
    memcpy(
        buf + layout.strOffsetsOffset,
        strings.offsets().data(),
        layout.strOffsetsBytes);
  }
  if (layout.strDataBytes != 0) {
    memcpy(
        buf + layout.strDataOffset, strings.data().data(), layout.strDataBytes);
  }
}

/// Serialize \p program, \p positions and \p strings into a freshly allocated
/// buffer. A convenience wrapper over \c containerLayout() and
/// \c writeContainerInto() for callers that do not have a destination of
/// their own; the addon writes directly into the result ArrayBuffer instead.
inline std::vector<uint8_t> writeContainer(
    const std::vector<uint32_t> &program,
    const std::vector<PositionResult> &positions,
    const NativeStringTable &strings) {
  const ContainerLayout layout = containerLayout(program, positions, strings);
  std::vector<uint8_t> buf(layout.total);
  writeContainerInto(buf.data(), layout, program, positions, strings);
  return buf;
}

} // namespace hermes

#endif
