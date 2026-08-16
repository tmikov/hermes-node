/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_MAPPED_FILE_H
#define HERMES_NODE_COMPAT_BUNDLE_MAPPED_FILE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace hermes {
namespace node_compat {

/// A whole file mapped read-only, unmapped when this object dies.
///
/// One copy of the open/fstat/mmap sequence for every consumer of a
/// container: the run path, which maps a bundle to execute bytecode out of
/// it, and the tools, which map one to describe or copy out of it. The two
/// differ in how long the mapping lives, not in how it is acquired, and
/// that difference is expressed by release() rather than by a second copy
/// of the syscalls.
///
/// POSIX only, like the rest of the container plumbing, and free of any
/// Hermes VM dependency -- this lives in the format layer, which
/// BundleFormatTest and BundleToolsTest both require to link with no
/// runtime.
class MappedFile {
 public:
  /// Returns std::nullopt and sets \p error if \p path cannot be opened or
  /// stat'ed, is not a regular file, or cannot be mapped.
  static std::optional<MappedFile> open(
      const std::string &path,
      std::string *error);

  MappedFile(MappedFile &&other) noexcept;
  MappedFile &operator=(MappedFile &&other) noexcept;
  MappedFile(const MappedFile &) = delete;
  MappedFile &operator=(const MappedFile &) = delete;
  ~MappedFile();

  /// Non-null for any instance open() returned, including for an empty
  /// file: mmap rejects a zero length, so an empty file yields a valid
  /// pointer and a size of 0 and is diagnosed by whichever reader consumes
  /// it as the truncated container it is. Null only for a moved-from
  /// instance, which owns nothing and views nothing.
  const uint8_t *data() const {
    return data_;
  }
  size_t size() const {
    return size_;
  }

  /// Gives up ownership of the mapping: it stays in place for the life of
  /// the process, and this object no longer unmaps it. data() and size()
  /// keep working, so the caller's views stay valid.
  ///
  /// For the run path (bundle_run.cpp), which executes bundled bytecode in
  /// place out of the mapping: those bytes have to outlive every reader,
  /// and in practice the process. A tool's mapping is not released,
  /// because nothing it reads outlives the call that reads it.
  void release() {
    mapping_ = nullptr;
  }

 private:
  MappedFile() = default;

  const uint8_t *data_ = nullptr;
  size_t size_ = 0;
  /// What to hand back to munmap, or nullptr when there is nothing to give
  /// back: an empty file, an instance that has been moved from, or one
  /// whose mapping was released.
  void *mapping_ = nullptr;
};

} // namespace node_compat
} // namespace hermes

#endif
