/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_BUNDLE_TOOLS_H
#define HERMES_NODE_COMPAT_BUNDLE_BUNDLE_TOOLS_H

// The mapping helper every tool verb starts from. Included rather than
// declared here so that one copy of the open/fstat/mmap sequence serves
// both the tools and the run path (bundle_run.cpp); it lives in the format
// layer, which both already link.
#include <hermes/node-compat/bundle/mapped_file.h>

#include <cstdint>
#include <ostream>
#include <string>

namespace hermes {
namespace node_compat {

/// Prints the container at \p bundlePath to \p out as text: the header, the
/// module table, the edge table, and the section sizes. Never bytecode
/// bytes, and never a byte of the bundled program's own output -- this
/// describes a container, it does not run one.
///
/// \p runningGeneration is the tag this binary would require to run the
/// container (bundleGenerationTag(), bundle_generation.h). It is passed in
/// rather than read here so that a test can drive a mismatch. A mismatch is
/// reported as a line of the dump and is not a failure: a container the
/// current binary refuses to run is exactly the one worth looking at.
///
/// \p verbose adds, per module, the number of edges pointing at it and the
/// number leaving it, which is what identifies the shared dependencies and
/// the leaves.
///
/// Returns 0 once anything was printed, or 1 with the reason on \p err if
/// the file cannot be mapped or fails the reader's structural validation.
int dumpBundle(
    const std::string &bundlePath,
    uint32_t runningGeneration,
    bool verbose,
    std::ostream &out,
    std::ostream &err);

/// Writes the payload of the module named \p identity, out of the
/// container at \p bundlePath, to \p outPath -- verbatim, no header, no
/// transformation. A JavaScript module's payload is the bytecode
/// hermes_compile_to_bytecode produced for it, so the file this writes is
/// directly loadable; a JSON module's payload is the source file's own
/// bytes, so the file this writes is byte-identical to it.
///
/// Opened in inspection mode, exactly like dumpBundle(): getting bytecode
/// out of a container the current binary refuses to run is a reason to
/// have this feature, not a reason to withhold it.
///
/// \p outPath is written by way of a temp file in its own directory,
/// renamed into place on success, so a failure at any point before the
/// rename leaves \p outPath untouched.
///
/// \p outPath naming the same file as \p bundlePath is refused before
/// anything is read or written. The rename would replace the container with
/// one module's payload -- a fraction of its own bytes -- and nothing
/// downstream would notice, because the reader is holding the old inode
/// through its mapping. Sameness is decided by (st_dev, st_ino) rather than
/// by comparing the two strings, so `./app.hbb`, a path through a symlinked
/// directory, a symlink to the container, and a second hard link to it are
/// all caught.
///
/// If no module in the container has identity \p identity, this reports
/// the miss on \p err together with up to three of the container's own
/// identities closest to it by Levenshtein distance -- only those within a
/// distance of a third of \p identity's length, so an unrelated identity
/// never appears just to pad out the list.
///
/// Returns 0 on success, or 1 with the reason on \p err: the file cannot
/// be mapped, fails the reader's structural validation, names no such
/// module, or the write itself fails.
int extractModule(
    const std::string &bundlePath,
    const std::string &identity,
    const std::string &outPath,
    std::ostream &err);

/// Checks each native addon the container at \p bundlePath records against
/// the file of that name in the container's own directory, printing one
/// line per addon to \p out and a summary to \p err.
///
/// Opened in inspection mode, like dumpBundle(): a container this binary
/// would refuse to run is still one whose sidecars are worth checking.
///
/// This is an AUDIT, NOT AN ENFORCEMENT. It reports what the files are at
/// the moment it runs; the program dlopens them later, and nothing here
/// closes the gap between the two. A cryptographic hash is used anyway --
/// SHA-256, not the CRC32 the generation tag uses -- because a CRC can be
/// forged to any value, and a check offered as a security step should not
/// have that as its weakest part.
///
/// Returns 0 when every recorded native is present and matches, and 1
/// otherwise -- so it can gate a deployment -- or when the container itself
/// cannot be read.
int verifyNatives(
    const std::string &bundlePath,
    bool verbose,
    std::ostream &out,
    std::ostream &err);

} // namespace node_compat
} // namespace hermes

#endif
