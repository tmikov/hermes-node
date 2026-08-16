/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/mapped_file.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace hermes {
namespace node_compat {

std::optional<MappedFile> MappedFile::open(
    const std::string &path,
    std::string *error) {
  auto fail = [&](const std::string &message) -> std::optional<MappedFile> {
    if (error != nullptr)
      *error = message;
    return std::nullopt;
  };

  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0)
    return fail("cannot open " + path + ": " + std::strerror(errno));

  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    std::string message = "cannot stat " + path + ": " + std::strerror(errno);
    ::close(fd);
    return fail(message);
  }
  if (!S_ISREG(st.st_mode)) {
    ::close(fd);
    return fail("not a regular file: " + path);
  }

  MappedFile result;
  result.size_ = static_cast<size_t>(st.st_size);

  // mmap rejects a zero length, and an empty file is a truncated container
  // like any other, so hand the caller a valid pointer and a size of 0 and
  // let the reader produce the message it produces for every other short
  // file. Never dereferenced, because every reader checks the size first.
  static const uint8_t kEmpty = 0;
  if (result.size_ == 0) {
    result.data_ = &kEmpty;
  } else {
    void *mapping =
        ::mmap(nullptr, result.size_, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapping == MAP_FAILED) {
      std::string message = "cannot map " + path + ": " + std::strerror(errno);
      ::close(fd);
      return fail(message);
    }
    // The mapping starts page-aligned, which is what preserves a
    // container's internal 8-byte payload alignment (kBundlePayloadAlign)
    // for bytecode executed in place.
    result.mapping_ = mapping;
    result.data_ = static_cast<const uint8_t *>(mapping);
  }

  // mmap keeps its own reference to the file, so the descriptor is done.
  ::close(fd);
  return std::optional<MappedFile>(std::move(result));
}

MappedFile::MappedFile(MappedFile &&other) noexcept
    : data_(other.data_), size_(other.size_), mapping_(other.mapping_) {
  other.data_ = nullptr;
  other.size_ = 0;
  other.mapping_ = nullptr;
}

MappedFile &MappedFile::operator=(MappedFile &&other) noexcept {
  if (this != &other) {
    if (mapping_ != nullptr)
      ::munmap(mapping_, size_);
    data_ = other.data_;
    size_ = other.size_;
    mapping_ = other.mapping_;
    other.data_ = nullptr;
    other.size_ = 0;
    other.mapping_ = nullptr;
  }
  return *this;
}

MappedFile::~MappedFile() {
  if (mapping_ != nullptr)
    ::munmap(mapping_, size_);
}

} // namespace node_compat
} // namespace hermes
