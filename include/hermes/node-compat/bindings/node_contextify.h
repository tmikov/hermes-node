/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BINDINGS_NODE_CONTEXTIFY_H
#define HERMES_NODE_COMPAT_BINDINGS_NODE_CONTEXTIFY_H

#include <node_api_types.h>

#include <cstddef>

namespace hermes {
namespace node_compat {

/// CJS module wrapper prefix prepended to source by
/// compileFunctionForCJSLoader. Matches Node's GetCJSParameters. The user
/// source is concatenated immediately after this string -- no leading
/// newline -- so user line N is wrapped line N. Exposed so the runtime
/// can compute the column offset where user code begins on line 1 (used
/// by --inspect-brk to set a breakpoint at the user's first instruction).
inline constexpr const char kCJSWrapperPrefix[] =
    "(function(exports, require, module, __filename, __dirname) {";

/// Number of characters in kCJSWrapperPrefix. CDP columnNumber is 0-based;
/// passing this value puts the breakpoint right at the first character of
/// user code on wrapped line 1.
inline constexpr size_t kCJSWrapperPrefixLen = sizeof(kCJSWrapperPrefix) - 1;

/// Init function for the 'contextify' binding.
/// Follows the napi_addon_register_func signature.
napi_value initContextifyBinding(napi_env env, napi_value exports);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BINDINGS_NODE_CONTEXTIFY_H
