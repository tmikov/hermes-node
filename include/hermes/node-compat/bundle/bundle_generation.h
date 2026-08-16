/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_BUNDLE_GENERATION_H
#define HERMES_NODE_COMPAT_BUNDLE_BUNDLE_GENERATION_H

#include <cstdint>
#include <string>
#include <string_view>

namespace hermes {
namespace node_compat {

/// The generation tag stamped into a bundle by BundleWriter::serialize and
/// checked by BundleReader::open (see bundle_writer.h, bundle_reader.h).
///
/// Derived the same way as the compile cache's generation name
/// (compileCacheGenerationName, compile_cache.h): a CRC-32 over the
/// hermes-node version string, the target architecture, the Hermes
/// bytecode version, and a marker byte for the optimization level. A
/// bundle built by a different hermes-node build, architecture, or
/// bytecode format therefore gets a different tag and is refused rather
/// than misread.
///
/// Stable for the lifetime of one build; not guaranteed stable across
/// builds, architectures, or hermes-node versions -- callers must not
/// persist it anywhere the container format itself does not already cover.
uint32_t bundleGenerationTag();

/// The same inputs bundleGenerationTag() folds into the tag, spelled out:
/// "hermes-node 0.1.0, x86_64, bytecode 96, optimized".
///
/// A tag is eight hex digits and says nothing about why two builds disagree.
/// This is what a producer prints next to it so that a later `--dump`
/// reporting a MISMATCH can be read against the build that stamped it --
/// version, architecture, bytecode format, or optimization level.
///
/// Deliberately built from the same four values in the same order as the
/// tag, in the same translation unit, so a field added to one is visible as
/// missing from the other.
std::string bundleGenerationDescription();

/// Composes a generation tag from explicit inputs, the way
/// bundleGenerationTag() composes it from the real ones. Exposed only so
/// BundleFormatTest can pin the CRC's field order and each field's contribution
/// without depending on the live build's version string, architecture, or
/// bytecode version. Production code should call bundleGenerationTag(), not
/// this.
uint32_t bundleGenerationTagFor(
    std::string_view version,
    std::string_view arch,
    uint32_t bytecodeVersion,
    char optimizeByte);

} // namespace node_compat
} // namespace hermes

#endif
