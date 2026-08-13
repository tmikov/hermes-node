/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <hermes/node-compat/compile-cache/compile_cache.h>
#include <hermes/node-compat/compile-cache/source_buffer.h>

#include <node_api.h>

#include <string_view>

namespace hermes {
namespace node_compat {

/// Produce a runnable value for a module's source, serving it from \p cache
/// when a valid entry exists and compiling, persisting and running it
/// otherwise.
///
/// What gets compiled is always what gets hashed, plus the caller's wrapper:
/// \p source is hashed and forms the body, and \p wrapPrefix / \p wrapSuffix
/// are concatenated around it before compiling. That invariant is what lets
/// the wrapper text live in the generation directory name instead of being
/// hashed per entry. Taking a finished string instead would let a caller key
/// an entry by text that did not produce it.
///
/// Compile and run flags are derived from \p kind rather than passed
/// alongside it; see flagsFor() in compile_cache_run.cpp for why that is a
/// correctness requirement for enable_ts.
///
/// Never opens or closes a handle scope; the caller owns scoping.
///
/// Returns napi_ok with \p result set on success. Returns a non-ok status
/// with the originating exception still pending when the user's own source
/// fails to compile, or when freshly compiled bytecode fails to run -- the
/// caller propagates both. A failure to run CACHED bytecode is never
/// reported: the exception is cleared, the entry deleted, and the source
/// recompiled, because a corrupt cache file must not surface as a
/// SyntaxError in valid user code.
napi_status compileCacheRun(
    napi_env env,
    CompileCache &cache,
    CompileCacheKind kind,
    const SourceBuffer &source,
    std::string_view wrapPrefix,
    std::string_view wrapSuffix,
    const char *sourceUrl,
    napi_value *result);

} // namespace node_compat
} // namespace hermes
