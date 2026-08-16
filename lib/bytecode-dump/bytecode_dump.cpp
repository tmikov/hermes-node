/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bytecode-dump/bytecode_dump.h>

// Two format constants, both header-only: the compile cache's entry magic
// and the bundle container's. Recognizing a file this binary wrote is the
// difference between a disassembly and a complaint about a bytecode magic
// that was never the problem. Neither costs a link dependency -- this
// target still links hermesvm_a and nothing else of ours.
#include <hermes/node-compat/bundle/bundle_format.h>
#include <hermes/node-compat/compile-cache/compile_cache.h>

#include "hermes/BCGen/HBC/BCProvider.h"
#include "hermes/BCGen/HBC/BytecodeDisassembler.h"
#include "hermes/BCGen/HBC/BytecodeFileFormat.h"
#include "hermes/BCGen/HBC/DisassemblyOptions.h"
#include "hermes/Support/MemoryBuffer.h"

#include "llvh/Support/MemoryBuffer.h"
#include "llvh/Support/raw_os_ostream.h"

#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace hermes {
namespace node_compat {

namespace {

/// A bytecode buffer that owns its bytes, allocated by operator new[] and so
/// aligned to at least alignof(std::max_align_t) -- comfortably more than
/// Hermes's BYTECODE_ALIGNMENT.
///
/// This exists for the compile cache case. Its bytecode starts one header
/// in, and a pointer part-way into someone else's allocation carries no
/// alignment guarantee at all, which BCProviderFromBuffer rejects outright
/// ("Buffer misaligned."). Copying is what makes the guarantee, and a
/// disassembly is a one-shot diagnostic, so the copy costs nothing worth
/// counting.
class OwnedCopyBuffer : public Buffer {
 public:
  OwnedCopyBuffer(const uint8_t *source, size_t size)
      : storage_(new uint8_t[size]) {
    std::memcpy(storage_.get(), source, size);
    data_ = storage_.get();
    size_ = size;
  }

 private:
  std::unique_ptr<uint8_t[]> storage_;
};

/// What the file turned out to be.
enum class InputKind {
  /// Nothing this tool recognizes sits in front: the whole file is bytecode,
  /// which is what an extracted module and a compiler output both are.
  kRawBytecode,
  /// A compile cache entry: bytecode behind a header this binary wrote.
  kCacheEntry,
  /// Carried the cache magic but failed the cache header's own checks. Kept
  /// apart from kRawBytecode because such a file is never valid bytecode
  /// either -- it starts with the cache magic, so the raw path would reject
  /// it for the one reason that is not true, a bad bytecode magic.
  kRejectedCacheEntry,
  /// A bundle container. The wrong tool, and worth saying so by name.
  kBundle,
};

struct Input {
  InputKind kind = InputKind::kRawBytecode;
  /// kCacheEntry: how many bytes of bytecode follow the header.
  size_t bytecodeSize = 0;
  /// kRejectedCacheEntry: which check the header failed.
  std::string reason;
};

/// Classifies \p data of \p size by the header it carries, if any.
///
/// Every cache header field is checked against the file's actual length
/// rather than trusted: the cache directory is user-writable, so a header
/// claiming more bytecode than the file holds is reachable, and following it
/// would read off the end of the mapping.
Input classifyInput(const uint8_t *data, size_t size) {
  Input input;

  if (size >= sizeof(kBundleMagic) &&
      std::memcmp(data, kBundleMagic, sizeof(kBundleMagic)) == 0) {
    input.kind = InputKind::kBundle;
    return input;
  }

  // memcpy rather than a cast: the header fields are little-endian uint32s
  // in a file, and the buffer's alignment is not this function's business.
  uint32_t magic = 0;
  if (size < sizeof(magic))
    return input;
  std::memcpy(&magic, data, sizeof(magic));
  if (magic != kCompileCacheMagic)
    return input;

  // From here the file claims to be a cache entry, so every failure below
  // describes the cache header rather than handing the file to Hermes to be
  // rejected for a bytecode magic that was never the problem.
  input.kind = InputKind::kRejectedCacheEntry;

  if (size < kCompileCacheHeaderSize) {
    input.reason = "truncated: " + std::to_string(size) +
        " bytes, shorter than the " + std::to_string(kCompileCacheHeaderSize) +
        "-byte entry header";
    return input;
  }

  uint32_t headerVersion = 0, bytecodeSize = 0;
  std::memcpy(&headerVersion, data + 4, sizeof(headerVersion));
  std::memcpy(&bytecodeSize, data + 16, sizeof(bytecodeSize));

  if (headerVersion != kCompileCacheHeaderVersion) {
    input.reason = "header version " + std::to_string(headerVersion) +
        ", this binary reads version " +
        std::to_string(kCompileCacheHeaderVersion);
    return input;
  }
  if (bytecodeSize == 0) {
    input.reason = "header records no bytecode";
    return input;
  }
  if (bytecodeSize > size - kCompileCacheHeaderSize) {
    input.reason = "header claims " + std::to_string(bytecodeSize) +
        " bytes of bytecode, but only " +
        std::to_string(size - kCompileCacheHeaderSize) +
        " bytes follow its header";
    return input;
  }

  input.kind = InputKind::kCacheEntry;
  input.bytecodeSize = bytecodeSize;
  return input;
}

} // namespace

int dumpBytecodeFile(
    const std::string &path,
    bool verbose,
    std::ostream &out,
    std::ostream &err) {
  llvh::ErrorOr<std::unique_ptr<llvh::MemoryBuffer>> fileOrErr =
      llvh::MemoryBuffer::getFile(path);
  if (!fileOrErr) {
    err << "error: " << path << ": " << fileOrErr.getError().message() << "\n";
    return 1;
  }

  std::unique_ptr<llvh::MemoryBuffer> file = std::move(fileOrErr.get());
  const auto *fileData =
      reinterpret_cast<const uint8_t *>(file->getBufferStart());
  size_t fileSize = file->getBufferSize();

  Input input = classifyInput(fileData, fileSize);
  bool fromCache = input.kind == InputKind::kCacheEntry;

  switch (input.kind) {
    case InputKind::kBundle:
      err << "error: " << path
          << ": this is a bundle container, not a file of bytecode\n"
          << "note: --bundle=" << path
          << " --dump lists its modules, and --bundle=" << path
          << " --extract-module=<identity> --out=<file> writes one out, "
             "which is a file this can disassemble\n";
      return 1;
    case InputKind::kRejectedCacheEntry:
      err << "error: " << path
          << ": compile cache entry rejected: " << input.reason << "\n";
      return 1;
    case InputKind::kCacheEntry:
    case InputKind::kRawBytecode:
      break;
  }

  std::unique_ptr<Buffer> buffer;
  if (fromCache) {
    buffer = std::make_unique<OwnedCopyBuffer>(
        fileData + kCompileCacheHeaderSize, input.bytecodeSize);
  } else {
    // The whole file is the bytecode, and llvh handed us an aligned
    // allocation for it, so this one can point straight at the mapping.
    buffer = std::make_unique<OwnedMemoryBuffer>(std::move(file));
  }

  // What the copy above is for, stated where a change can trip over it. No
  // test can tell a copy from a pointer 24 bytes into the mapping today --
  // the header is a multiple of 8, so both come out aligned on every
  // platform we build -- which is exactly why dropping the copy as an
  // obvious optimization would pass the whole suite and break only where
  // the header size or BYTECODE_ALIGNMENT differs. BCProviderFromBuffer
  // rejects a misaligned buffer outright ("Buffer misaligned.").
  assert(
      reinterpret_cast<uintptr_t>(buffer->data()) % hbc::BYTECODE_ALIGNMENT ==
          0 &&
      "bytecode buffer must satisfy BYTECODE_ALIGNMENT");

  // Captured before the buffer moves into the provider: the section walker
  // reports offsets relative to the start of the bytecode, so it needs the
  // same address the provider parsed from.
  const uint8_t *bytecodeStart = buffer->data();
  size_t bytecodeSize = buffer->size();

  auto result =
      hbc::BCProviderFromBuffer::createBCProviderFromBuffer(std::move(buffer));
  if (!result.first) {
    // Hermes's own diagnosis, verbatim. It already distinguishes a file too
    // short to hold a header from one whose magic is wrong from one whose
    // version this binary cannot read, and restating any of that here would
    // only be a second place for it to go stale.
    err << "error: " << path << ": " << result.second << "\n";
    return 1;
  }
  std::shared_ptr<hbc::BCProviderFromBuffer> provider = std::move(result.first);

  out << "bytecode: " << path << "  (" << bytecodeSize << " bytes"
      << (fromCache ? ", compile cache entry)" : ")") << "\n\n";
  // The disassembler writes through llvh's stream; flush before and after so
  // the two never interleave out of order on the way to the same fd.
  out.flush();

  {
    llvh::raw_os_ostream os(out);

    // Pretty is the readable form, function ids and virtual offsets are what
    // make a line in it referable to from elsewhere. Source lines are the one
    // part gated on --verbose: they multiply the line count and are only as
    // good as the debug info the file happens to carry.
    hbc::DisassemblyOptions options = hbc::DisassemblyOptions::Pretty |
        hbc::DisassemblyOptions::IncludeFunctionIds |
        hbc::DisassemblyOptions::IncludeVirtualOffsets;
    if (verbose)
      options = options | hbc::DisassemblyOptions::IncludeSource;

    hbc::BytecodeDisassembler disassembler(provider);
    disassembler.setOptions(options);
    disassembler.disassemble(os);

    os << "\n";
    // Where the bytes went. Printed after the disassembly rather than
    // before it because it is the summary of what was just listed.
    hbc::BytecodeSectionWalker walker(bytecodeStart, provider, os);
    walker.printSectionRanges(/* human */ false);
  }

  out.flush();
  return 0;
}

} // namespace node_compat
} // namespace hermes
