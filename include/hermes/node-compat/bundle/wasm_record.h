/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_WASM_RECORD_H
#define HERMES_NODE_COMPAT_BUNDLE_WASM_RECORD_H

#include <hermes/node-compat/bundle/bundle_format.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes {
namespace node_compat {

/// Eight bytes, no NUL, like kBundleMagic.
constexpr char kWasmRecordMagic[8] = {'H', 'N', 'W', 'A', 'S', 'M', 'R', 'C'};

/// Bumped when the layout changes. A mismatch is a hard error at bake time.
constexpr uint32_t kWasmRecordFormatVersion = 1;

/// Fixed-width. Everything variable is reached through an offset, so that
/// the record table is always 4-byte aligned however long the build version
/// string is -- a length-prefixed string written inline ahead of the table
/// would leave it at an arbitrary offset, and reading a uint32 there is
/// undefined behaviour whatever x86-64 tolerates.
struct WasmRecordHeader {
  char magic[8];
  uint32_t formatVersion;
  uint32_t count;
  uint32_t versionOffset;
  uint32_t versionLength;
  uint32_t recordTableOffset;
  uint32_t payloadOffset;
  uint32_t payloadSize;
};

/// Accumulates the compiled bytecode of every distinct WebAssembly module a
/// run saw, keyed by the compile cache's own digest, and rewrites one
/// `--record-wasm` file. Deliberately free of Hermes and napi headers, like
/// BundleWriter, so the format can be unit tested with no runtime.
///
/// Unlike BundleWriter, which accumulates in memory and is serialized once
/// at the end of a build, this writer persists after every record(): the
/// process it runs inside has no "end of build" moment (a script that
/// instantiates ten Wasm modules and is killed after the fifth should not
/// lose those five), so every call that changes what the file would contain
/// rewrites it on the spot, atomically, through writeFileAtomically().
class WasmRecordWriter {
 public:
  /// \p path is where record() and flush() write; \p buildVersion is stored
  /// verbatim and is the string a later --build-bundle --bake-wasm compares
  /// against its own build's version before trusting an entry's bytecode.
  WasmRecordWriter(std::string path, std::string buildVersion);

  /// Add or replace the entry for \p digest, then rewrite the file. \p
  /// digest is kNativeDigestBytes (32) raw bytes, the same SHA-256 the
  /// compile cache computes over the codegen configuration and the module
  /// bytes.
  ///
  /// Replacing rather than keeping the first is required, not incidental: a
  /// lookup records the bytes it returned, and Hermes may then REJECT them
  /// and compile fresh bytecode instead. The store that follows must
  /// overwrite what the lookup recorded, or the next bake carries the bad
  /// entry forward.
  ///
  /// A record whose digest and bytes already match rewrites nothing: it
  /// costs a linear scan and a memcmp against one entry, not a write.
  ///
  /// Returns false if the file could not be written; the entry is still
  /// kept in memory, so a later record() or flush() gets another chance --
  /// unless that later call is an exact repeat of an entry already held,
  /// which rewrites nothing.
  bool record(const uint8_t *digest, const uint8_t *bytecode, size_t size);

  /// Write the file as it stands, including with no entries. Called once
  /// when the RUNTIME is created -- after the flag-conflict checks, before
  /// user code runs -- so that a --record-wasm target that is not writable
  /// fails before any Wasm module has been compiled, rather than silently
  /// losing the first one. Not at flag-parse time: an earlier write could
  /// destroy a same-named input before a same-file guard has run.
  bool flush();

  /// Why the most recent record() or flush() returned false: one complete
  /// line, already ending in a newline, naming the file and the errno text.
  /// Empty when nothing has failed.
  ///
  /// An accessor rather than an out-parameter because the two callers want
  /// different things from it -- the runtime prints it, a test asserts on it
  /// -- and because the failure is not always at the call the caller cares
  /// about: the writer keeps unwritten entries in memory and retries on the
  /// next record(), so "did this call succeed" and "why did writing fail"
  /// are separate questions.
  const std::string &lastError() const {
    return lastError_;
  }

 private:
  /// Serializes the current entries and writes them to path_ via
  /// writeFileAtomically(). Shared by record() and flush() so there is one
  /// copy of the layout logic.
  bool writeFile();

  struct PendingRecord {
    std::string digest; // kNativeDigestBytes raw bytes
    std::string bytecode;
  };

  std::string path_;
  std::string buildVersion_;
  std::vector<PendingRecord> records_;
  /// See lastError(). Cleared by a write that succeeds, so it always
  /// describes the most recent failure and never an older one.
  std::string lastError_;
};

/// A read-only view over a mapped `--record-wasm` file. Does not own the
/// bytes; the caller keeps the mapping (or buffer) alive for the reader's
/// lifetime.
///
/// open() validates every offset, length and alignment in the header
/// against \p size before any accessor can be called, so accessors perform
/// no bounds checks of their own beyond index range -- the same contract
/// BundleReader::open() makes.
///
/// Validation is structural only, exactly like the container: magic,
/// format version, and every offset, length, alignment and range. It never
/// verifies payload bytes. There is no checksum in this format, and none
/// should be added -- see the comment on BundleWasmRecord in
/// bundle_format.h for why.
class WasmRecordReader {
 public:
  /// Returns std::nullopt and sets \p error on bad magic, format version
  /// mismatch, truncation, or any out-of-range or misaligned offset.
  static std::optional<WasmRecordReader>
  open(const uint8_t *data, size_t size, std::string *error);

  /// The build version string the writer was constructed with.
  std::string_view buildVersion() const;

  uint32_t count() const;

  /// The raw kNativeDigestBytes-byte digest of record \p i. Only valid for
  /// \p i below count(), with no runtime check, exactly like BundleReader's
  /// per-index accessors.
  std::string_view digest(uint32_t i) const;

  /// The compiled bytecode of record \p i. Only valid for \p i below
  /// count(), exactly like digest() above.
  std::string_view payload(uint32_t i) const;

 private:
  WasmRecordReader() = default;

  const uint8_t *data_ = nullptr;
  const WasmRecordHeader *header_ = nullptr;
  const BundleWasmRecord *records_ = nullptr;
};

} // namespace node_compat
} // namespace hermes

#endif
