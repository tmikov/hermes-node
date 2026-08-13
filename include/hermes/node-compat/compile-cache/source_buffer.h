/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <string>

namespace hermes {
namespace node_compat {

/// A read-only source range that records whether a NUL byte follows its
/// contents.
///
/// SIZE NEVER COUNTS THE TERMINATOR. size() is the length of the source text
/// alone; when isNulTerminated() is true the NUL lives at data()[size()],
/// one past the last source byte, and is readable. This is deliberately the
/// opposite of the hermes_compile_to_bytecode convention, where the caller
/// folds the terminator into the size it passes. The two are easy to confuse
/// -- which is the whole reason this class exists -- so the difference is
/// stated here and asserted below.
///
/// Hermes compiles zero-copy from a terminated buffer and copies internally
/// otherwise. A memory-mapped file satisfies it for free, since the kernel
/// zero-fills the tail of the final page, except when the file length is an
/// exact multiple of the page size. Neither const std::string & nor
/// std::string_view can express the distinction -- the first demands an
/// owning terminated heap string, the second discards the information.
///
/// Only destruction is virtual, because ownership is the only thing that
/// differs between a mapping, a heap block and a std::string. The accessors
/// read members directly.
class SourceBuffer {
 public:
  virtual ~SourceBuffer() = default;

  SourceBuffer(const SourceBuffer &) = delete;
  SourceBuffer &operator=(const SourceBuffer &) = delete;

  const char *data() const {
    return data_;
  }

  /// Length of the source text, never counting the terminator.
  size_t size() const {
    return size_;
  }

  /// True when data()[size()] is readable and is '\0'.
  bool isNulTerminated() const {
    return nulTerminated_;
  }

  /// Number of bytes readable at data(). Includes the terminator when this
  /// buffer has one, so it is size() + 1 for a terminated buffer and size()
  /// otherwise. APIs that want the terminator counted in the length they are
  /// given take this instead of size().
  size_t readableSize() const {
    return nulTerminated_ ? size_ + 1 : size_;
  }

 protected:
  SourceBuffer(const char *data, size_t size, bool nulTerminated)
      : data_(data), size_(size), nulTerminated_(nulTerminated) {
    assert((data != nullptr || size == 0) && "null data with nonzero size");
    // Reads the byte the caller just promised is readable. A caller that
    // lies fails here, rather than silently extending the compiled text by
    // one byte or reading out of bounds deep inside Hermes.
    assert(
        (!nulTerminated || data[size] == '\0') &&
        "isNulTerminated set but data[size] is not 0");
  }

 private:
  const char *const data_;
  const size_t size_;
  const bool nulTerminated_;
};

/// Borrows a std::string. c_str() guarantees the terminator; the assert in
/// the base checks it rather than assuming it.
class BorrowedStringSourceBuffer final : public SourceBuffer {
 public:
  explicit BorrowedStringSourceBuffer(const std::string &s)
      : SourceBuffer(s.data(), s.size(), true) {}
};

} // namespace node_compat
} // namespace hermes
