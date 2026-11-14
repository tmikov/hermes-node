/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string_view>

namespace hermes {
namespace node_compat {

/// The specifier prefix that names an embedder module -- e.g.
/// "hermes-node:ui". See EmbedderModule (hermes_node_runtime.h) for what a
/// specifier under this prefix resolves to.
///
/// This constant lives in its own header, separate from
/// hermes_node_runtime.h, so that lib/bundle -- which must stay free of NAPI
/// and the Hermes VM (BundleFormatTest and friends run with no runtime) --
/// can share it without pulling in <node_api_types.h>. Both C++ sites
/// (bundle_resolve.cpp's isBuiltinSpecifier and hermes-node.cpp's
/// --test-embedder-module registration) include this header rather than
/// hermes_node_runtime.h.
///
/// Kept in sync by hand with the identical literal in libjs/loader.js
/// (where a require() is resolved to one) and libjs/bundle-loader.js (so a
/// bundled require() forwards it back to the loader instead of treating it
/// as a missing edge). Nothing forces the JS copies to agree with this one;
/// see the "closed world" bundle notes in CLAUDE.md for a case where two
/// such lists already drifted.
///
/// RULING 36: "hermes-node:" is a distinct scheme from Node's own "node:",
/// not an alternate spelling that stacks with it. "node:hermes-node:x" is
/// rejected everywhere -- a doubled scheme is a spelling with no meaning --
/// so every site that tests for this prefix must test the RAW request,
/// never a request that has already had a leading "node:" stripped from it.
inline constexpr std::string_view kEmbedderModulePrefix = "hermes-node:";

} // namespace node_compat
} // namespace hermes
