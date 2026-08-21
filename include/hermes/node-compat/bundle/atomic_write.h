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

/// True if \p a and \p b both exist and name the same file -- same device,
/// same inode.
///
/// Compares identity rather than spelling because the spellings that reach
/// the same file are unbounded: "app.hbb" and "./app.hbb", a relative and an
/// absolute path, a path through a symlinked directory, a symlink to the
/// container, and a second hard link to it. stat() follows symlinks, so all
/// of those collapse to one comparison here.
///
/// A path that cannot be stat()'d (most often because it does not exist,
/// which is the normal case for an output file) is not the same file as
/// anything, so the answer is false and the caller carries on.
///
/// Lives beside writeFileAtomically because both of its callers are about
/// to write a file and are asking whether the write would land somewhere it
/// must not: the extractor refusing to write a module's payload over its
/// own container (bundle_tools.cpp), and the producer refusing to copy a
/// native addon over the bundle (bundle_build.cpp). Two copies of a
/// refusal rule is how the two come to disagree.
///
/// A caller asking the opposite question -- "is there nothing to do?", as
/// the producer does when an addon already IS its own sidecar -- needs a
/// symlink check of its own before trusting a true answer here. A symlink
/// at the destination pointing at the source is the same file by this
/// predicate, but skipping the write there leaves a link into the source
/// tree where a real file was meant to be.
bool isSameFile(const std::string &a, const std::string &b);

} // namespace node_compat
} // namespace hermes

#endif
