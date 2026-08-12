/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "hermes/AST/Context.h"
#include "hermes/AST/ESTree.h"
#include "hermes/Parser/FlowHelpers.h"
#include "hermes/Parser/JSParser.h"
#include "hermes/Sema/SemContext.h"
#include "hermes/Sema/SemResolve.h"

#include "llvh/ADT/StringRef.h"
#include "llvh/Support/MemoryBuffer.h"

#include "ContainerWriter.h"
#include "HermesParserDiagHandler.h"
#include "HermesParserJSSerializer.h"

#include "node_api.h"

using namespace hermes;

namespace {

/// Read an optional boolean property from \p obj, defaulting to false.
bool boolOption(napi_env env, napi_value obj, const char *name) {
  napi_value prop;
  if (napi_get_named_property(env, obj, name, &prop) != napi_ok) {
    return false;
  }

  napi_valuetype type;
  if (napi_typeof(env, prop, &type) != napi_ok || type != napi_boolean) {
    return false;
  }

  bool value = false;
  if (napi_get_value_bool(env, prop, &value) != napi_ok) {
    return false;
  }
  return value;
}

/// Set \p name on \p obj to the given uint32 value.
void setUint32(napi_env env, napi_value obj, const char *name, uint32_t v) {
  napi_value num;
  if (napi_create_uint32(env, v, &num) == napi_ok) {
    napi_set_named_property(env, obj, name, num);
  }
}

/// Set \p name on \p obj to the given UTF-8 string.
void setString(
    napi_env env,
    napi_value obj,
    const char *name,
    const std::string &v) {
  napi_value str;
  if (napi_create_string_utf8(env, v.data(), v.size(), &str) == napi_ok) {
    napi_set_named_property(env, obj, name, str);
  }
}

/// Build the `{error, line, column}` descriptor. The caller in JavaScript
/// turns this into a SyntaxError; Node-API cannot construct one directly.
napi_value errorResult(
    napi_env env,
    const std::string &message,
    uint32_t line,
    uint32_t column) {
  napi_value obj;
  if (napi_create_object(env, &obj) != napi_ok) {
    napi_throw_error(env, nullptr, "failed to allocate error result object");
    return nullptr;
  }
  setString(env, obj, "error", message);
  setUint32(env, obj, "line", line);
  setUint32(env, obj, "column", column);
  return obj;
}

/// \return true if any comment in \p context's doc block for \p fileBufId
/// contains an `@flow` pragma.
bool hasFlowPragma(Context &context, uint32_t fileBufId) {
  std::vector<parser::StoredComment> comments =
      parser::getCommentsInDocBlock(context, fileBufId);
  return parser::hasFlowPragma(comments);
}

/// Parse a source string and return either `{buffer}` or
/// `{error, line, column}`.
///
/// The Context/parser setup and error-handling lifecycle below is copied
/// verbatim (adjusted for Node-API instead of Emscripten exports) from
/// tools/hermes-parser/hermes-parser-wasm.cpp, which is the working
/// reference for how these options and diagnostics are wired up.
napi_value parse(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) {
    napi_throw_error(env, nullptr, "failed to read call arguments");
    return nullptr;
  }
  if (argc < 2) {
    napi_throw_type_error(
        env, nullptr, "parse(source, options) requires two arguments");
    return nullptr;
  }

  // Copy the source out as NUL-terminated UTF-8. napi_get_value_string_utf8
  // NUL-terminates for us, so the parser's zero-termination requirement is
  // satisfied without a separate guard.
  size_t sourceLen = 0;
  if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &sourceLen) !=
      napi_ok) {
    napi_throw_type_error(env, nullptr, "source must be a string");
    return nullptr;
  }
  std::vector<char> source(sourceLen + 1, '\0');
  if (napi_get_value_string_utf8(
          env, argv[0], source.data(), source.size(), &sourceLen) != napi_ok) {
    napi_throw_error(env, nullptr, "failed to read source");
    return nullptr;
  }

  const bool detectFlow = boolOption(env, argv[1], "detectFlow");
  const bool componentSyntax =
      boolOption(env, argv[1], "enableExperimentalComponentSyntax");
  const bool matchSyntax =
      boolOption(env, argv[1], "enableExperimentalFlowMatchSyntax");
  const bool recordSyntax =
      boolOption(env, argv[1], "enableExperimentalFlowRecordSyntax");
  const bool tokens = boolOption(env, argv[1], "tokens");
  const bool allowReturnOutsideFunction =
      boolOption(env, argv[1], "allowReturnOutsideFunction");

  // Set up custom diagnostic handler for error reporting.
  auto context = std::make_shared<Context>();
  auto &sm = context->getSourceErrorManager();
  const auto &diagHandler = HermesParserDiagHandler(sm);

  // Declared after \c diagHandler so that it is destroyed first. \c result
  // owns the parser and a reference to the context, whose destructors run
  // against the SourceErrorManager that \c diagHandler is registered on; the
  // handler must therefore still be alive at that point.
  ParseResult result;

  auto fileBuf = llvh::MemoryBuffer::getMemBuffer(
      llvh::StringRef{source.data(), sourceLen});
  int fileBufId = sm.addNewSourceBuffer(std::move(fileBuf));

  auto parseFlowSetting = detectFlow && !hasFlowPragma(*context, fileBufId)
      ? ParseFlowSetting::UNAMBIGUOUS
      : ParseFlowSetting::ALL;
  context->setParseFlow(parseFlowSetting);
  context->setParseFlowComponentSyntax(componentSyntax);
  context->setParseFlowMatch(matchSyntax);
  context->setParseFlowRecords(recordSyntax);
  context->setParseJSX(true);
  context->setUseCJSModules(true);
  context->setAllowReturnOutsideFunction(allowReturnOutsideFunction);

  std::unique_ptr<parser::JSParser> jsParser =
      std::make_unique<parser::JSParser>(
          *context, fileBufId, parser::FullParse);
  jsParser->setStoreComments(true);
  jsParser->setStoreTokens(tokens);

  llvh::Optional<ESTree::ProgramNode *> parsedJs = jsParser->parse();

  // Return the first error if any were detected during parsing.
  if (diagHandler.hasError()) {
    return errorResult(
        env,
        diagHandler.getErrorString(),
        diagHandler.getErrorLine(),
        diagHandler.getErrorColumn());
  }

  // Return a generic error if no AST was produced but no specific error was
  // detected.
  if (!parsedJs) {
    return errorResult(env, "Failed to parse source", 0, 0);
  }

  // Keep the context and parser alive on the result: serialize() below
  // dereferences result.parser_ (e.g. for comments/tokens).
  result.context_ = context;
  result.parser_ = std::move(jsParser);
  serialize(*parsedJs, &sm, result, tokens);

  // Run semantic validation after the AST has been serialized. This mirrors
  // the reference (tools/hermes-parser/hermes-parser-wasm.cpp): resolution
  // never changes the already-serialized AST bytes, but it does reject
  // programs that parse syntactically yet are semantically invalid (e.g.
  // `continue` outside a loop), which must surface as parse errors too.
  sema::SemContext semContext{*context};
  resolveASTForParser(*context, semContext, *parsedJs);

  // Return the first error if any were detected during semantic validation.
  if (diagHandler.hasError()) {
    return errorResult(
        env,
        diagHandler.getErrorString(),
        diagHandler.getErrorLine(),
        diagHandler.getErrorColumn());
  }

  // Size the container first, then write it straight into the ArrayBuffer's
  // storage. Going through an intermediate std::vector would cost an extra
  // full copy of the result plus a zero-fill of a buffer that is then
  // completely overwritten.
  const ContainerLayout layout = containerLayout(
      result.programBuffer_, result.positionBuffer_, result.stringTable_);

  void *data = nullptr;
  napi_value arrayBuffer;
  if (napi_create_arraybuffer(env, layout.total, &data, &arrayBuffer) !=
      napi_ok) {
    napi_throw_error(env, nullptr, "failed to allocate result buffer");
    return nullptr;
  }
  writeContainerInto(
      data,
      layout,
      result.programBuffer_,
      result.positionBuffer_,
      result.stringTable_);

  napi_value obj;
  if (napi_create_object(env, &obj) != napi_ok) {
    napi_throw_error(env, nullptr, "failed to allocate result object");
    return nullptr;
  }
  napi_set_named_property(env, obj, "buffer", arrayBuffer);
  return obj;
}

/// Module initializer. Registers `parse` on the exports object.
napi_value init(napi_env env, napi_value exports) {
  napi_value fn;
  if (napi_create_function(
          env, "parse", NAPI_AUTO_LENGTH, parse, nullptr, &fn) != napi_ok) {
    napi_throw_error(env, nullptr, "failed to create the parse function");
    return nullptr;
  }
  if (napi_set_named_property(env, exports, "parse", fn) != napi_ok) {
    napi_throw_error(env, nullptr, "failed to export the parse function");
    return nullptr;
  }
  return exports;
}

} // namespace

NAPI_MODULE(NODE_GYP_MODULE_NAME, init)
