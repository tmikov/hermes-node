/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BYTECODE_DUMP_BYTECODE_DUMP_H
#define HERMES_NODE_COMPAT_BYTECODE_DUMP_BYTECODE_DUMP_H

#include <ostream>
#include <string>

namespace hermes {
namespace node_compat {

/// Disassembles the Hermes bytecode in the file at \p path to \p out: the
/// bytecode file header, the string and literal tables, every function's
/// instructions, and the byte range of each section.
///
/// Two file shapes are accepted, because those are the two this binary
/// produces. A raw bytecode file, such as --extract-module writes out of a
/// container, is the normal case. A compile cache entry -- the same
/// bytecode behind a fixed-size header (lib/compile-cache) -- is accepted
/// too, detected by that header's magic and skipped past; a cache directory
/// is full of them, and telling a user to hexdump past a header this binary
/// wrote itself would be a poor answer.
///
/// \p verbose adds the source line and column of each instruction, which
/// requires the debug info the file may or may not carry.
///
/// Nothing here executes the bytecode, and nothing here needs a runtime:
/// the file is read, parsed as a container of bytecode, and printed.
///
/// Returns 0 once the disassembly was printed, or 1 with the reason on
/// \p err -- the file cannot be read, or Hermes rejects its contents, in
/// which case Hermes's own diagnosis is what gets reported rather than a
/// second-hand paraphrase of it.
int dumpBytecodeFile(
    const std::string &path,
    bool verbose,
    std::ostream &out,
    std::ostream &err);

} // namespace node_compat
} // namespace hermes

#endif
