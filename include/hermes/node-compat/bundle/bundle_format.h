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
constexpr uint32_t kBundleFormatVersion = 5;

/// Every payload entry starts at a multiple of this. Hermes bytecode is
/// executed in place from the mapping and requires alignment.
constexpr size_t kBundlePayloadAlign = 8;

enum class ModuleKind : uint32_t {
  kJavaScript = 0,
  kJSON = 1,
  /// A native addon. Its bytes are NOT in the container: they ship as a
  /// flat sidecar file next to the bundle, because dlopen() takes a path
  /// and there is no portable way to load a shared object from memory.
  /// payloadSize is zero (the reader rejects anything else); payloadOffset
  /// carries no meaning for a zero-size entry and is whatever position the
  /// writer's payload cursor happened to be at when it reached this module,
  /// same as any other zero-byte module would get. Everything else about
  /// the addon lives in the native table (BundleNativeRecord below).
  kNative = 2,
};

/// Set on a `BundleModuleRecord` whose module `require()` may load. Clear
/// on a record that exists only so the resolver can read it -- a
/// `package.json` consulted for its `main` field but never itself
/// `require()`d -- which must stay invisible to `require()` even though its
/// bytes are in the container.
constexpr uint32_t kRequirable = 1u << 0;

/// The absence of `kRequirable`. Named so a resolution-input record's
/// `flags` reads as a deliberate choice, not a leftover zero.
constexpr uint32_t kResolveOnly = 0;

/// Set on a container whose recorded VM options may be overridden at run
/// time, by --vm= on a --bundle= command line or by HERMES_NODE_VM_OPTIONS
/// for a linked executable. Clear -- the default -- means the options are
/// a property of the artifact and an attempt to override them is an error,
/// because the honoured flag set includes -enable-eval and
/// -Xhermes-internal-test-methods, which are not tuning knobs.
constexpr uint32_t kBundleFlagAllowVmOptionsOverride = 1u << 0;

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
  // The preload table: an array of uint32_t module indices, one per
  // preload, in the order the modules must run before the entry point.
  // Order is the whole point of this table -- it is why a preload is a
  // section of its own rather than another flag bit on the module record,
  // which could say "this module preloads" but not "before that one".
  uint32_t preloadTableOffset;
  uint32_t preloadCount;
  // The native table: an array of BundleNativeRecord, sorted by
  // moduleIndex, one per kNative module. A section of its own rather than
  // three more fields on every module record: a real bundle has ~1500
  // modules and one or two natives, so the record would carry twelve bytes
  // of zeros per module to describe the exception. This mirrors the
  // preload table's reasoning above.
  uint32_t nativeTableOffset;
  uint32_t nativeCount;
  // The VM-options table: an array of uint32_t string indices, one per
  // recorded Hermes VM option, in the order they are applied -- later
  // wins, which is llvh::cl's own rule. A section of its own for the same
  // reason the preload table is one: order is the whole content, and a
  // flag bit on a module record could not express it. String indices
  // rather than inline text so the existing string table does the storage.
  uint32_t vmOptionsTableOffset;
  uint32_t vmOptionsCount;
  // Container-wide flags: currently only kBundleFlagAllowVmOptionsOverride.
  // Distinct from BundleModuleRecord::flags, which is per module.
  uint32_t containerFlags;
  uint32_t payloadOffset;
  uint32_t payloadSize;
};

/// One per packaged module. `identityString` indexes the string table.
struct BundleModuleRecord {
  uint32_t identityString;
  uint32_t kind; // ModuleKind
  uint32_t flags; // bitwise OR of kRequirable and (currently) nothing else
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

/// Length in bytes of a native record's digest, as stored in the string
/// table entry `hashString` points at. Raw SHA-256 output, not hex text: 32
/// bytes either way, but the format declares its own field width rather
/// than importing picohash's PICOHASH_SHA256_DIGEST_LENGTH into this header
/// -- this header itself stays free of that compile-time coupling, so the
/// reader's check and the producer's output agree on a number this file
/// states, not on a constant reached through picohash. (hermesNodeBundle,
/// the library both live in, does link picohash already, for
/// native_digest.cpp's own use; this constant is about not reaching for
/// picohash's header a second time, not about the link graph.) See the
/// static_assert next to native_digest.cpp's use of this constant, which is
/// what keeps the two declarations of "32" from silently drifting apart.
constexpr size_t kNativeDigestBytes = 32;

/// One per kNative module. `sidecarString` and `hashString` index the
/// string table; the hash entry holds the raw kNativeDigestBytes-byte
/// SHA-256, not hex, so it may contain NUL bytes -- which the string
/// table's explicit length prefix already allows.
///
/// `byteLength` and the digest are recorded at build time and read by
/// nothing on the run path: hashing at load would mean reading the whole
/// addon on every launch, in an artifact whose reason for existing is
/// startup cost. They exist for --dump and --verify-natives.
struct BundleNativeRecord {
  uint32_t moduleIndex;
  uint32_t sidecarString;
  uint32_t byteLength;
  uint32_t hashString;
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
