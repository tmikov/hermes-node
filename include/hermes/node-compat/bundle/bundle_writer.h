/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_BUNDLE_WRITER_H
#define HERMES_NODE_COMPAT_BUNDLE_BUNDLE_WRITER_H

#include <hermes/node-compat/bundle/bundle_format.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hermes {
namespace node_compat {

/// Accumulates modules and edges, then serializes one container.
///
/// Deliberately free of Hermes and napi headers so the format can be unit
/// tested with no runtime, exactly as CompileCache is.
class BundleWriter {
 public:
  /// Copies \p payload. \p flags is a bitwise OR of the module-record flag
  /// bits (see bundle_format.h), typically just kRequirable. Returns the new
  /// module's index.
  uint32_t addModule(
      std::string_view identity,
      ModuleKind kind,
      uint32_t flags,
      std::string_view payload);

  /// Records that \p importer resolved \p specifier to \p target.
  void addEdge(uint32_t importer, std::string_view specifier, uint32_t target);

  /// Records that \p moduleIndex must run before the entry point. Call
  /// order is preload order -- see the preload table's documentation in
  /// bundle_format.h.
  void addPreload(uint32_t moduleIndex);

  /// Records the sidecar file that carries module \p moduleIndex's bytes.
  /// \p rawDigest is the raw SHA-256 (32 bytes), not hex. Call order does
  /// not matter: serialize() sorts the table by module index, which is what
  /// BundleReader::nativeFor() binary-searches.
  void addNative(
      uint32_t moduleIndex,
      std::string_view sidecar,
      uint32_t byteLength,
      std::string_view rawDigest);

  /// Records a Hermes VM option. Call order is application order: later
  /// occurrences of the same flag win, so this is a list rather than a set
  /// and duplicates are kept.
  void addVmOption(std::string_view option);

  /// Records whether the container's VM options may be overridden at run
  /// time. False -- the default -- means locked.
  void setAllowVmOptionsOverride(bool allow);

  void setEntry(uint32_t moduleIndex);

  /// Returns the serialized container. Sorts the edge table by
  /// (importer, specifier bytes) as the reader's binary search requires.
  /// That sort is not stable, which is unobservable: the producer records
  /// one edge per (importer, specifier) pair, because scanRequires()
  /// deduplicates the specifiers of a single file (see require_scanner.h),
  /// so no two edges ever compare equal.
  /// Returns an empty vector if no module was ever added.
  std::vector<uint8_t> serialize(uint32_t generationTag);

  /// How many distinct strings the string table holds. Module identities and
  /// edge specifiers share one interning table, so this is neither the
  /// module count nor the edge count, and a shared specifier or an identity
  /// that equals a specifier is counted once.
  ///
  /// Only complete after serialize(): identities are interned as modules are
  /// added, but specifiers are not interned until serialize() lays out the
  /// table. The header records the table's byte size, not its entry count,
  /// so this is the only place the count exists.
  size_t stringCount() const;

 private:
  /// Interns \p s, returning its byte offset within the string table.
  uint32_t internString(std::string_view s);

  struct PendingModule {
    uint32_t identityString;
    ModuleKind kind;
    uint32_t flags;
    std::string payload;
  };
  struct PendingEdge {
    uint32_t importer;
    std::string specifier;
    uint32_t target;
  };
  struct PendingNative {
    uint32_t moduleIndex;
    std::string sidecar;
    uint32_t byteLength;
    std::string digest;
  };

  std::vector<PendingModule> modules_;
  std::vector<PendingEdge> edges_;
  std::vector<uint32_t> preloads_;
  std::vector<PendingNative> natives_;
  std::vector<std::string> vmOptions_;
  std::map<std::string, uint32_t, std::less<>> internTable_;
  std::string stringBytes_;
  uint32_t entry_ = 0;
  bool hasEntry_ = false;
  bool allowVmOptionsOverride_ = false;
};

} // namespace node_compat
} // namespace hermes

#endif
