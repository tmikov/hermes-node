/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_TOOLS_HERMESPARSERNATIVE_STRINGTABLE_H
#define HERMES_TOOLS_HERMESPARSERNATIVE_STRINGTABLE_H

#include <cstdint>
#include <string>
#include <vector>

#include "llvh/ADT/DenseMap.h"
#include "llvh/ADT/StringRef.h"

namespace hermes {

/// A deduplicating table of UTF-8 strings, serialized as a single blob of
/// bytes plus an offset array.
///
/// String \c i occupies <tt>data()[offsets()[i] .. offsets()[i+1]]</tt>, so
/// \c offsets() always holds one more entry than there are strings and no
/// separate length array is needed.
///
/// Keys are stored as \c StringRef, so callers must guarantee the referenced
/// bytes outlive the table. Every current caller satisfies this: identifiers
/// live in the parser's identifier table and comment and token text point
/// into the source buffer, both of which are owned by the \c Context that
/// outlives serialization.
///
/// Named \c NativeStringTable rather than \c StringTable to avoid colliding
/// with \c hermes::StringTable (include/hermes/Support/StringTable.h), which
/// is pulled in transitively by any translation unit that also includes
/// hermes-parser-native's forked serializer headers (via ESTree.h /
/// JSParser.h). Both classes live directly in \c namespace \c hermes,
/// matching the flat-namespace convention already used by the forked
/// hermes-parser sources.
class NativeStringTable {
 public:
  /// Add \p str to the table, or return the existing id if already present.
  /// \return the id of the string, counting from zero.
  uint32_t intern(llvh::StringRef str) {
    auto it = index_.find(str);
    if (it != index_.end()) {
      return it->second;
    }

    uint32_t id = (uint32_t)(offsets_.size() - 1);
    data_.append(str.data(), str.size());
    offsets_.push_back((uint32_t)data_.size());
    index_.try_emplace(str, id);
    return id;
  }

  /// \return the concatenated UTF-8 bytes of every interned string.
  const std::string &data() const {
    return data_;
  }

  /// \return the offset array, with \c count() + 1 entries.
  const std::vector<uint32_t> &offsets() const {
    return offsets_;
  }

  /// \return the number of distinct strings interned.
  uint32_t count() const {
    return (uint32_t)(offsets_.size() - 1);
  }

 private:
  llvh::DenseMap<llvh::StringRef, uint32_t> index_{};
  std::vector<uint32_t> offsets_{0};
  std::string data_{};
};

} // namespace hermes

#endif
