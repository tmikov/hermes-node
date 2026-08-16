/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUNDLE_CJS_WRAPPER_H
#define HERMES_NODE_COMPAT_BUNDLE_CJS_WRAPPER_H

#include <cstdint>
#include <string>
#include <string_view>

namespace hermes {
namespace node_compat {

/// The module wrapper libjs/loader.js applies to a user file read from disk
/// (see loadModule() there).
///
/// Every JavaScript module compiled into a bundle is wrapped identically, so
/// a compiled function value invoked as
/// (exports, require, module, __filename, __dirname) behaves the same way
/// whether the loader got it from the bundle or from a fresh disk compile.
///
/// The require() scanner wraps the source with this too, so that it parses
/// and resolves exactly the text the compiler will see. Two things depend on
/// that. A module body is a function body, so constructs that are legal only
/// inside a function -- a top-level `return`, which is an ordinary early-exit
/// idiom in CommonJS -- are legal here and are a syntax error when the same
/// text is parsed as a Program. And `require` is a parameter of this wrapper,
/// which is what makes "is this identifier the module's require" a question
/// about a binding rather than about a name.
///
/// The prefix deliberately ends without a newline: wrapped line N is source
/// line N, and only line 1 is shifted, by exactly kCJSWrapperPrefix.size()
/// columns. See unwrapCoords().
constexpr std::string_view kCJSWrapperPrefix =
    "(function(exports, require, module, __filename, __dirname) {";
constexpr std::string_view kCJSWrapperSuffix = "\n})";

/// \return \p source wrapped in the CommonJS module wrapper.
inline std::string wrapCJS(std::string_view source) {
  std::string wrapped;
  wrapped.reserve(
      kCJSWrapperPrefix.size() + source.size() + kCJSWrapperSuffix.size());
  wrapped.append(kCJSWrapperPrefix);
  wrapped.append(source);
  wrapped.append(kCJSWrapperSuffix);
  return wrapped;
}

/// Converts a 1-based (\p line, \p column) in the wrapped text back to the
/// same position in the original source, in place.
inline void unwrapCoords(uint32_t line, uint32_t *column) {
  if (line != 1)
    return;
  // A position inside the prefix itself cannot come from the source, and
  // clamping rather than underflowing keeps a bad one merely wrong instead
  // of enormous.
  *column = *column > kCJSWrapperPrefix.size()
      ? *column - static_cast<uint32_t>(kCJSWrapperPrefix.size())
      : 1;
}

} // namespace node_compat
} // namespace hermes

#endif
