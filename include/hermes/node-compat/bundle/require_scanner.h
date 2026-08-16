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

/// Where a `require()` whose argument is not a literal was written.
///
/// Such a call is invisible to static discovery -- the specifier only exists
/// once the program computes it -- so whatever it names is not packaged, and
/// is left to the run-time fallback. That is a correctness-preserving
/// outcome and a hole in the container at the same time, and the position
/// recorded here is the only thing that can point at it.
struct ComputedRequire {
  /// 1-based, as the parser reports them.
  uint32_t line = 0;
  uint32_t column = 0;
};

/// Parses \p source with the Hermes parser and appends every literal
/// `require()` argument to \p out, in source order, with duplicates removed
/// (first occurrence wins). A call counts only when its callee is a bare
/// identifier named exactly `require` (so `require.resolve(...)` and
/// `obj.require(...)`, both MemberExpression callees, are ignored) and its
/// first argument is a string literal, or a template literal with exactly
/// one quasi and no substitutions.
///
/// Does not resolve specifiers or touch the filesystem; it only reports the
/// raw strings written in the source.
///
/// \param enableTS parse the source as TypeScript.
/// \param out appended to; not cleared first.
/// \param error set to a description of the failure on a parse error.
/// \param computed when non-null, appended to with the position of every
///     `require()` call this scan had to give up on because its argument is
///     not a literal. A call with no arguments at all is not one of these:
///     it names nothing, computed or otherwise. Optional because a caller
///     that only wants the specifiers has no use for the positions, and
///     collecting them means decoding source locations.
/// \return false and sets \p error on a parse error; true otherwise (even if
///     no require() calls were found).
bool scanRequires(
    std::string_view source,
    bool enableTS,
    std::vector<std::string> *out,
    std::string *error,
    std::vector<ComputedRequire> *computed = nullptr);

} // namespace node_compat
} // namespace hermes

#endif
