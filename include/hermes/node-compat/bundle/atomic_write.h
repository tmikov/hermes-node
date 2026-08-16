/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_ATOMIC_WRITE_H
#define HERMES_NODE_COMPAT_BUNDLE_ATOMIC_WRITE_H

#include <cstddef>
#include <ostream>
#include <string>

namespace hermes {
namespace node_compat {

/// Writes [data, data + size) to \p outPath by way of a temp file in the
/// same directory (outPath plus a ".<pid>.<counter>.tmp" suffix), which is
/// written, closed, and only then rename()'d into place. A failure at any
/// point before the rename -- opening the temp file, writing to it, or
/// closing it -- leaves \p outPath untouched and the temp file removed.
///
/// The close() is checked, not just the write()s: some filesystems (NFS,
/// some FUSE mounts) defer a write error to close() rather than reporting
/// it eagerly, and a failure there means the temp file's content cannot be
/// trusted, so it must be treated exactly like a failed write rather than
/// let through to the rename.
///
/// One copy of this sequence for every writer of a whole file that needs
/// this guarantee: the bundle producer (the whole container, in
/// bundle_build.cpp) and the bundle tools' extractor (one module's
/// payload, in bundle_tools.cpp). Lives in the format layer
/// (hermesNodeBundle), which both already link, the same way MappedFile is
/// one copy of the read side's open/fstat/mmap.
///
/// Returns true on success; on failure, reports the reason to \p err.
bool writeFileAtomically(
    const std::string &outPath,
    const void *data,
    size_t size,
    std::ostream &err);

} // namespace node_compat
} // namespace hermes

#endif
