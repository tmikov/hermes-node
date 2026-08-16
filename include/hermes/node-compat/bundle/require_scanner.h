/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_REQUIRE_SCANNER_H
#define HERMES_NODE_COMPAT_BUNDLE_REQUIRE_SCANNER_H

#include <string>
#include <string_view>
#include <vector>

namespace hermes {
namespace node_compat {

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
/// \return false and sets \p error on a parse error; true otherwise (even if
///     no require() calls were found).
bool scanRequires(
    std::string_view source,
    bool enableTS,
    std::vector<std::string> *out,
    std::string *error);

} // namespace node_compat
} // namespace hermes

#endif
