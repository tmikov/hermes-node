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

/// The language features every JavaScript file in a bundle is compiled
/// with. Three things have to agree about them: the require() scanner
/// (lib/bundle/require_scanner.cpp), the bytecode compile step, and -- for
/// a native build -- the shermes command line.
///
/// This struct is the source for TWO of the three. The bytecode step
/// hardcodes the same values inside Hermes
/// (hermes/API/napi/hermes_napi_compile.cpp), which a header here cannot
/// reach; widening hermes_compile_flags so the producer supplies them would
/// close that, and has not been done. Until it is, what keeps the third in
/// line is test/bundle-async-generator.js and
/// test/build-native-parity.js, not this declaration. If you change a value
/// here, change it there too.
///
/// The scanner's Context used to take Hermes's own defaults, which disable
/// async generators and ES6 block scoping. Async generators are a PARSE
/// error when disabled, so the scanner rejected modules the compiler behind
/// it accepted and the producer packaged them as throwing stubs: a program
/// that ran from disk threw once bundled. See dz 01a09e14-0a0e.
///
/// Block scoping is set here for a weaker reason: it does not change
/// anything the scanner computes. `Context::getEnableES6BlockScoping()` is
/// read in exactly one place in the whole Hermes tree,
/// hermes/lib/IRGen/ESTreeIRGen-stmt.cpp (loop-capture codegen) -- nothing
/// under lib/Sema/, lib/AST/ or lib/Parser/ consults it, and
/// `SemanticResolver::visit(BlockStatementNode*)` pushes a new scope for
/// every `{ }` unconditionally, flag or no flag. The scanner runs the
/// parser and `sema::resolveAST` and never reaches IRGen, so
/// `setEnableES6BlockScoping` below is inert on this path today; confirmed
/// by removing the call and rebuilding, which changed nothing about the
/// require()-shadowing case in test/bundle-async-generator.js. It stays set
/// for two reasons anyway: to track the compiler's configuration in case
/// Sema ever starts consulting it, and because this same struct will drive
/// the shermes command line, where the flag IS load-bearing.
struct JSLanguageFlags {
  bool es6BlockScoping = true;
  bool asyncGenerators = true;
  bool generators = true;
  /// The one field that is per file rather than per project: set when the
  /// module's extension is `.ts`.
  bool typeScript = false;
};

/// The project-wide settings, with TypeScript off. A caller with a `.ts`
/// file copies this and sets `typeScript`.
constexpr JSLanguageFlags kJSLanguageFlags{};

} // namespace node_compat
} // namespace hermes

#endif
