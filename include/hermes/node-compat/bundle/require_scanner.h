/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_REQUIRE_SCANNER_H
#define HERMES_NODE_COMPAT_BUNDLE_REQUIRE_SCANNER_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hermes {
namespace node_compat {

/// Why a use of the module's `require` could not be followed statically.
enum class RequireGapKind {
  /// `require(expr)`: the specifier only exists once the program computes
  /// it, so what it names is unknown here.
  kComputedArgument,
  /// `require` used as a value rather than called -- passed to a function,
  /// assigned, returned. Everything it goes on to load is unknown, and
  /// unlike a computed argument there is not even a call site to point at.
  /// `@babel/core` does this: `endHiddenCallStack(require)(filepath)`.
  kEscapedValue,
};

/// One use of the module's `require` that static discovery cannot follow.
///
/// Whatever such a use names is not packaged and is left to the run-time
/// fallback -- correct while the source tree is present, and a hole in the
/// container once it is not. The position is the only thing that can point
/// at it.
struct RequireGap {
  RequireGapKind kind = RequireGapKind::kComputedArgument;
  /// 1-based, and relative to the original source, not to the CommonJS
  /// wrapper the scan puts around it.
  uint32_t line = 0;
  uint32_t column = 0;
};

/// Wraps \p source in the CommonJS module wrapper (see cjs_wrapper.h),
/// parses and semantically resolves it with the Hermes front end, and
/// appends every literal `require()` argument to \p out, in source order,
/// with duplicates removed (first occurrence wins).
///
/// A call counts only when its callee is an identifier **bound to the
/// wrapper's `require` parameter** -- that is, to the real module require --
/// and its first argument is a string literal, or a template literal with
/// exactly one quasi and no substitutions. Resolving the binding rather than
/// matching the name is what keeps a module that declares its own inner
/// `require` (the shape browserify and older webpack output ship) from
/// contributing specifiers that were only ever meaningful inside that
/// module's own bundle.
///
/// `require.resolve(...)` and `obj.require(...)` are not calls of `require`
/// and are ignored, as they were before.
///
/// Does not resolve specifiers or touch the filesystem; it only reports the
/// raw strings written in the source.
///
/// \param enableTS parse the source as TypeScript.
/// \param out appended to; not cleared first.
/// \param error set to a description of the failure on a parse or resolution
///     error. Positions in it are relative to the wrapped text, which is
///     what the compiler's own diagnostics already do.
/// \param gaps when non-null, appended to with every use of the module's
///     `require` this scan could not follow -- see RequireGap. A `require()`
///     with no arguments at all is not one: it names nothing, computed or
///     otherwise. Optional because a caller that only wants the specifiers
///     has no use for the positions, and collecting them means decoding
///     source locations.
/// \return false and sets \p error on a parse or resolution error; true
///     otherwise (even if no require() calls were found).
bool scanRequires(
    std::string_view source,
    bool enableTS,
    std::vector<std::string> *out,
    std::string *error,
    std::vector<RequireGap> *gaps = nullptr);

} // namespace node_compat
} // namespace hermes

#endif
