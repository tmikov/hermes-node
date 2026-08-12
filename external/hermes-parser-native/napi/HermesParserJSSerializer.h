/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_TOOLS_HERMESPARSERNATIVE_HERMESPARSERJSSERIALIZER_H
#define HERMES_TOOLS_HERMESPARSERNATIVE_HERMESPARSERJSSERIALIZER_H

#include "hermes/AST/ESTree.h"
#include "hermes/Parser/JSParser.h"

#include "StringTable.h"

namespace hermes {

/// General category for token, based off esprima's token types.
enum class TokenType {
  Boolean,
  Identifier,
  Keyword,
  Null,
  Numeric,
  BigInt,
  Punctuator,
  String,
  RegularExpression,
  Template,
  JSXText
};

/// Holder for the kind of a source position, kept as a struct so that the
/// enumerators keep naming which end of a range they refer to.
struct PositionInfo {
  /// Which end of a source range a position refers to.
  enum class Kind { Start, End };
};

/// One resolved endpoint of a source range, as written into the container.
///
/// Entries appear in the order the serializer walks the AST: both endpoints
/// of loc 0, then both endpoints of loc 1, and so on. The consumer indexes
/// them by \c locId (HermesParserDeserializer.fillLocs), so nothing requires
/// any particular order of them.
struct PositionResult {
  /// ID of the source location this position is associated with.
  uint32_t locId;
  /// 0 if this is a start position, 1 if this is an end position.
  uint32_t kind;

  uint32_t line;
  uint32_t column;
  uint32_t offset;

  PositionResult(
      uint32_t locId,
      PositionInfo::Kind kind,
      uint32_t line,
      uint32_t column,
      uint32_t offset)
      : locId(locId),
        kind(kind == PositionInfo::Kind::Start ? 0 : 1),
        line(line),
        column(column),
        offset(offset) {}
};

/// An opaque object containing the result of parsing
class ParseResult {
 public:
  std::string error_;
  uint32_t errorLine_ = 0;
  uint32_t errorColumn_ = 0;
  /// Buffer containing serialized AST
  std::vector<uint32_t> programBuffer_;
  /// Buffer containing serialized source positions
  std::vector<PositionResult> positionBuffer_;

  // Keep references to parser and context as they should last until
  // parse result is freed.
  std::shared_ptr<Context> context_{nullptr};
  std::unique_ptr<parser::JSParser> parser_{nullptr};

  /// Deduplicated table of every string referenced by the program buffer.
  ///
  /// Declared after \c context_ and \c parser_ so that it is destroyed
  /// *before* them. Its keys are \c StringRef s pointing into the identifier
  /// table and the source buffer, both owned by the \c Context; see the
  /// lifetime requirement documented on NativeStringTable in StringTable.h.
  NativeStringTable stringTable_;
};

void serialize(
    ESTree::ProgramNode *programNode,
    SourceErrorManager *sm,
    ParseResult &result,
    bool tokens);

} // namespace hermes

#endif
