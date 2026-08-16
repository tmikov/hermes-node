/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_BUNDLE_FORMAT_H
#define HERMES_NODE_COMPAT_BUNDLE_BUNDLE_FORMAT_H

#include <cstddef>
#include <cstdint>

namespace hermes {
namespace node_compat {

/// Eight bytes, no NUL. A JavaScript file cannot begin with these.
constexpr char kBundleMagic[8] = {'H', 'N', 'B', 'U', 'N', 'D', 'L', 'E'};

/// Bumped whenever the layout below changes in a way older readers cannot
/// interpret. A mismatch is a hard error; there is no forward compatibility.
constexpr uint32_t kBundleFormatVersion = 1;

/// Every payload entry starts at a multiple of this. Hermes bytecode is
/// executed in place from the mapping and requires alignment.
constexpr size_t kBundlePayloadAlign = 8;

enum class ModuleKind : uint32_t {
  kJavaScript = 0,
  kJSON = 1,
};

/// Fixed-width. Offsets are byte offsets from the start of the file.
struct BundleHeader {
  char magic[8];
  uint32_t formatVersion;
  uint32_t generationTag;
  uint32_t entryModule;
  uint32_t stringsOffset;
  uint32_t stringsSize;
  uint32_t moduleTableOffset;
  uint32_t moduleCount;
  uint32_t edgeTableOffset;
  uint32_t edgeCount;
  uint32_t payloadOffset;
  uint32_t payloadSize;
};

/// One per packaged module. `identityString` indexes the string table.
struct BundleModuleRecord {
  uint32_t identityString;
  uint32_t kind; // ModuleKind
  uint32_t payloadOffset; // from payloadOffset in the header
  uint32_t payloadSize;
};

/// One per resolved (importer, specifier) pair. Sorted by importer index,
/// then by the *bytes* of the specifier -- see bundle_reader.h for why.
struct BundleEdgeRecord {
  uint32_t importer;
  uint32_t specifierString;
  uint32_t target;
};

/// Entries in the string table are stored as uint32 length followed by that
/// many bytes, with no NUL. A string index is the byte offset of its length
/// field from stringsOffset.
struct BundleStringHeader {
  uint32_t length;
};

} // namespace node_compat
} // namespace hermes

#endif
